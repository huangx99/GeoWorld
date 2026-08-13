#include "geoworld/gateway/stream_transport.hpp"

#include "geoworld/gateway/control_params.hpp"
#include "geoworld/gateway/errors.hpp"
#include "geoworld/gateway/frame_codec.hpp"
#include "geoworld/protocol/codec.hpp"
#include "geoworld/protocol/error.hpp"
#include "geoworld/protocol/version.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/stream.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace geoworld::gateway {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

// ticket 由服务端 TokenGenerator 生成，约定为 URL 安全字符，查询串不做百分号解码。
[[nodiscard]] std::optional<std::string> extract_ticket(std::string_view target,
                                                        std::string_view stream_path) {
    const std::size_t query = target.find('?');
    if (query == std::string_view::npos || target.substr(0, query) != stream_path) {
        return std::nullopt;
    }
    const std::string_view rest = target.substr(query + 1);
    std::size_t begin = 0;
    while (begin <= rest.size()) {
        const std::size_t end = rest.find('&', begin);
        const std::string_view pair =
            rest.substr(begin, end == std::string_view::npos ? end : end - begin);
        const std::size_t eq = pair.find('=');
        if (eq != std::string_view::npos
            && pair.substr(0, eq) == stream_ticket_query_key) {
            return std::string{pair.substr(eq + 1)};
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return std::nullopt;
}

[[nodiscard]] bool offers_subprotocol(
    const http::request<http::string_body>& request) {
    const auto header = request.find(http::field::sec_websocket_protocol);
    if (header == request.end()) {
        return false;
    }
    const std::string_view offered{header->value().data(), header->value().size()};
    return offered.find(protocol::websocket_subprotocol) != std::string_view::npos;
}

template <typename Stream>
struct is_ssl_stream : std::false_type {};

template <>
struct is_ssl_stream<beast::ssl_stream<beast::tcp_stream>> : std::true_type {};

// 101 响应必须回显协商的 subprotocol，否则客户端拒绝升级结果。
template <typename Stream>
void apply_subprotocol_decorator(websocket::stream<Stream>& stream) {
    stream.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& response) {
            response.set(http::field::sec_websocket_protocol,
                         protocol::websocket_subprotocol);
        }));
}

class shard_session;

// 分片线程 -> 组合根主线程的单向事件。attach/frame/closed 按分片内产生顺序入队；
// 携带会话 shared_ptr，保证主线程处理期间会话对象存活。
struct inbound_event {
    enum class Kind { attach, frame, closed };

    Kind kind{Kind::frame};
    projection::ConnectionId connection;
    FrameBytes bytes;
    std::shared_ptr<shard_session> session;

    [[nodiscard]] static inbound_event attach(projection::ConnectionId connection,
                                              std::shared_ptr<shard_session> session) {
        return inbound_event{Kind::attach, connection, {}, std::move(session)};
    }
    [[nodiscard]] static inbound_event frame(projection::ConnectionId connection,
                                             FrameBytes bytes,
                                             std::shared_ptr<shard_session> session) {
        return inbound_event{Kind::frame, connection, std::move(bytes),
                             std::move(session)};
    }
    [[nodiscard]] static inbound_event closed(projection::ConnectionId connection,
                                              std::shared_ptr<shard_session> session) {
        return inbound_event{Kind::closed, connection, {}, std::move(session)};
    }
};

class ws_session_base {
public:
    virtual ~ws_session_base() = default;

    virtual void start() = 0;
    virtual void close() = 0;
    virtual void enqueue(FrameBytes bytes) = 0;
    [[nodiscard]] virtual bool closed() const noexcept = 0;
    [[nodiscard]] virtual std::size_t queued_bytes() const noexcept = 0;
    // 升级请求校验后 socket 已移交分片的会话返回 true：主线程清扫时不得 detach 核心连接。
    [[nodiscard]] virtual bool transferred() const noexcept { return false; }
};

struct session_owner {
    virtual ~session_owner() = default;

    [[nodiscard]] virtual GatewayCore& core() noexcept = 0;
    [[nodiscard]] virtual const protocol::ProtocolLimits& limits() const noexcept = 0;
    // 分片模式：升级请求校验通过（ticket 已 attach 到核心）后，明文 socket 与
    // 升级请求一并移交归属分片，由分片线程完成 websocket accept；返回 false 表示未移交。
    [[nodiscard]] virtual bool sharding_enabled() const noexcept = 0;
    virtual bool handoff_to_shard(
        projection::ConnectionId connection, tcp::socket&& socket,
        http::request<http::string_body>&& request) = 0;
    // 分片线程事件入口；实现只入加锁队列，不在此触碰 GatewayCore。
    virtual void push_inbound(inbound_event event) = 0;
};

// 单连接状态机：TLS 握手（可选）-> HTTP 升级校验（subprotocol + ticket）->
// 二进制帧读写。所有回调运行在拥有者 poll() 的同一线程，可直接调用 GatewayCore。
template <typename Stream>
class ws_session final : public ws_session_base,
                         public std::enable_shared_from_this<ws_session<Stream>> {
public:
    ws_session(Stream&& stream, session_owner& owner,
               projection::ConnectionId connection, std::string stream_path)
        : ws_(std::move(stream)), owner_(owner), connection_(connection),
          stream_path_(std::move(stream_path)) {
        ws_.read_message_max(owner_.limits().max_frame_bytes);
    }

    void start() override {
        auto self = this->shared_from_this();
        beast::get_lowest_layer(ws_).expires_after(handshake_timeout);
        if constexpr (is_ssl_stream<Stream>::value) {
            ws_.next_layer().async_handshake(
                asio::ssl::stream_base::server,
                [self](beast::error_code error) { self->on_handshake(error); });
        } else {
            read_request();
        }
    }

    void close() override {
        if (closing_ || closed_) {
            return;
        }
        closing_ = true;
        if (!writing_) {
            do_close();
        }
    }

    void enqueue(FrameBytes bytes) override {
        queued_bytes_ += bytes.size();
        outbound_.push_back(std::move(bytes));
        if (!writing_ && !closed_ && accepted_) {
            write_next();
        }
    }

    [[nodiscard]] bool closed() const noexcept override { return closed_; }
    [[nodiscard]] std::size_t queued_bytes() const noexcept override {
        return queued_bytes_;
    }
    [[nodiscard]] bool transferred() const noexcept override { return transferred_; }

private:
    static constexpr std::chrono::seconds handshake_timeout{30};

    void on_handshake(beast::error_code error) {
        if (error) {
            mark_closed();
            return;
        }
        read_request();
    }

    void read_request() {
        auto self = this->shared_from_this();
        http::async_read(ws_.next_layer(), buffer_, request_,
                         [self](beast::error_code error, std::size_t) {
                             self->on_request(error);
                         });
    }

    void on_request(beast::error_code error) {
        if (error) {
            mark_closed();
            return;
        }
        if (!websocket::is_upgrade(request_) || !offers_subprotocol(request_)) {
            reject(http::status::bad_request, protocol::error_invalid_request);
            return;
        }
        const std::optional<std::string> ticket =
            extract_ticket(request_.target(), stream_path_);
        if (!ticket.has_value()) {
            reject(http::status::bad_request, protocol::error_invalid_request);
            return;
        }
        if (!owner_.core().attach_stream(connection_, *ticket).has_value()) {
            reject(http::status::forbidden, protocol::error_ticket_invalid);
            return;
        }
        if constexpr (!is_ssl_stream<Stream>::value) {
            // 分片模式：在发送 101 之前移交 socket 与升级请求，websocket accept
            // 由归属分片线程完成（beast 新建 ws 流默认 status::closed，只有
            // accept/handshake 成功才会进入 open，必须让分片上的新流自己 accept）。
            // 移交失败时 socket 随局部变量关闭，主线程清扫会 detach 核心连接。
            if (owner_.sharding_enabled()) {
                tcp::socket socket{std::move(beast::get_lowest_layer(ws_).socket())};
                if (owner_.handoff_to_shard(connection_, std::move(socket),
                                            std::move(request_))) {
                    transferred_ = true;
                }
                mark_closed();
                return;
            }
        }
        beast::get_lowest_layer(ws_).expires_never();
        apply_subprotocol_decorator(ws_);
        auto self = this->shared_from_this();
        ws_.async_accept(request_, [self](beast::error_code accept_error) {
            self->on_accept(accept_error);
        });
    }

    void reject(http::status status, std::string_view code) {
        auto response = std::make_shared<http::response<http::string_body>>(
            status, request_.version());
        response->set(http::field::content_type, "text/plain");
        response->body() = std::string{code};
        response->prepare_payload();
        auto self = this->shared_from_this();
        http::async_write(ws_.next_layer(), *response,
                          [self, response](beast::error_code, std::size_t) {
                              beast::error_code ignored;
                              beast::get_lowest_layer(self->ws_).socket().shutdown(
                                  tcp::socket::shutdown_both, ignored);
                              self->mark_closed();
                          });
    }

    void on_accept(beast::error_code error) {
        if (error) {
            mark_closed();
            return;
        }
        accepted_ = true;
        ws_.binary(true);
        if (!outbound_.empty() && !writing_) {
            write_next();
        }
        read_next();
    }

    void read_next() {
        auto self = this->shared_from_this();
        ws_.async_read(buffer_, [self](beast::error_code error, std::size_t) {
            self->on_read(error);
        });
    }

    void on_read(beast::error_code error) {
        if (error) {
            mark_closed();
            return;
        }
        // flat_buffer::data() 返回单段 const_buffer，直接取裸指针与长度。
        const auto* bytes =
            static_cast<const std::uint8_t*>(buffer_.data().data());
        protocol::DecodeFailure failure;
        const std::optional<protocol::WireClientControl> control =
            protocol::decode_client_control(
                std::span<const std::uint8_t>{bytes, buffer_.size()},
                failure, owner_.limits());
        buffer_.consume(buffer_.size());
        if (!control.has_value()) {
            // 畸形输入：回送稳定错误码协议错误帧后关闭，不再读下一帧。
            outbound_.push_front(
                encode_protocol_error(failure.error_code, failure.message,
                                      owner_.limits()));
            queued_bytes_ += outbound_.front().size();
            closing_ = true;
            if (!writing_) {
                write_next();
            }
            return;
        }
        if (const auto* ack = std::get_if<protocol::WireAck>(&*control)) {
            static_cast<void>(
                owner_.core().inbound_ack(connection_, ack->stream_epoch,
                                          ack->snapshot_id));
        } else {
            static_cast<void>(owner_.core().inbound_keyframe_request(connection_));
        }
        read_next();
    }

    void write_next() {
        if (outbound_.empty()) {
            writing_ = false;
            if (closing_) {
                do_close();
            }
            return;
        }
        writing_ = true;
        ws_.binary(true);
        auto self = this->shared_from_this();
        ws_.async_write(
            asio::buffer(outbound_.front().data(), outbound_.front().size()),
            [self](beast::error_code error, std::size_t) {
                self->on_write(error);
            });
    }

    void on_write(beast::error_code error) {
        if (error) {
            mark_closed();
            return;
        }
        queued_bytes_ -= outbound_.front().size();
        outbound_.pop_front();
        write_next();
    }

    void do_close() {
        auto self = this->shared_from_this();
        ws_.async_close(websocket::close_code::normal,
                        [self](beast::error_code) { self->mark_closed(); });
    }

    void mark_closed() {
        closed_ = true;
        beast::error_code ignored;
        beast::get_lowest_layer(ws_).socket().close(ignored);
    }

    websocket::stream<Stream> ws_;
    session_owner& owner_;
    projection::ConnectionId connection_;
    std::string stream_path_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    std::deque<FrameBytes> outbound_;
    std::size_t queued_bytes_{};
    bool accepted_{};
    bool writing_{};
    bool closing_{};
    bool closed_{};
    bool transferred_{};
};

using plain_session = ws_session<beast::tcp_stream>;
using tls_session = ws_session<beast::ssl_stream<beast::tcp_stream>>;

struct shard_context;

// 分片连接会话：全部 socket 回调只运行在归属分片线程，绝不访问 GatewayCore。
// 并发边界：主线程经 enqueue/request_close 跨线程写入，共享状态仅 outbox 与
// close 标志（mutex 保护）；入站帧经 owner.push_inbound 移交主线程解码处理。
// websocket accept 在本会话上完成：ticket 校验与核心 attach 已在主线程完成，
// 但 beast 新建的 ws 流默认 status::closed，只有自己的 accept 成功才进入 open。
class shard_session final : public std::enable_shared_from_this<shard_session> {
public:
    shard_session(tcp::socket&& socket,
                  http::request<http::string_body>&& request, session_owner& owner,
                  projection::ConnectionId connection, shard_context* shard)
        : ws_(beast::tcp_stream{std::move(socket)}), owner_(owner),
          connection_(connection), shard_(shard), request_(std::move(request)) {
        ws_.read_message_max(owner_.limits().max_frame_bytes);
    }

    // ---- 跨线程入口（组合根主线程调用）----

    void enqueue(FrameBytes bytes) {
        {
            std::lock_guard lock{mutex_};
            outbox_bytes_ += bytes.size();
            outbox_.push_back(std::move(bytes));
        }
        // kick 去重：已有一次 drain 在途时不重复 post。
        if (!kick_pending_.exchange(true, std::memory_order_acq_rel)) {
            auto self = shared_from_this();
            asio::post(ws_.get_executor(), [self] { self->drain_outbox(); });
        }
    }

    void request_close() {
        {
            std::lock_guard lock{mutex_};
            if (close_requested_) {
                return;
            }
            close_requested_ = true;
        }
        auto self = shared_from_this();
        asio::post(ws_.get_executor(), [self] { self->handle_close_request(); });
    }

    [[nodiscard]] std::size_t outbox_bytes() const {
        std::lock_guard lock{mutex_};
        return outbox_bytes_;
    }

    [[nodiscard]] shard_context* shard() const noexcept { return shard_; }
    [[nodiscard]] projection::ConnectionId connection() const noexcept {
        return connection_;
    }

    // ---- 分片线程入口 ----

    void start() {
        // beast::tcp_stream 新建时 op_state 定时器到期点为 epoch，未显式
        // expires_* 的首次读写会被当作已超时并关闭 socket；分片连接不做
        // 应用层超时，显式关闭流超时。
        beast::get_lowest_layer(ws_).expires_never();
        apply_subprotocol_decorator(ws_);
        auto self = shared_from_this();
        ws_.async_accept(request_, [self](beast::error_code error) {
            self->on_accept(error);
        });
    }

private:
    void on_accept(beast::error_code error) {
        if (error) {
            // accept 失败：核心连接已在主线程 attach，经 closed 事件 detach。
            report_closed();
            return;
        }
        ws_.binary(true);
        owner_.push_inbound(inbound_event::attach(connection_, shared_from_this()));
        read_next();
    }

    void read_next() {
        auto self = shared_from_this();
        ws_.async_read(buffer_, [self](beast::error_code error, std::size_t) {
            self->on_read(error);
        });
    }

    void on_read(beast::error_code error) {
        if (error) {
            report_closed();
            return;
        }
        // 原始帧字节移交主线程：解码与核心调用都在主线程完成。
        const auto* bytes =
            static_cast<const std::uint8_t*>(buffer_.data().data());
        FrameBytes frame(buffer_.size());
        std::memcpy(frame.data(), bytes, frame.size());
        buffer_.consume(buffer_.size());
        owner_.push_inbound(
            inbound_event::frame(connection_, std::move(frame), shared_from_this()));
        read_next();
    }

    void drain_outbox() {
        // 先放行下一次 kick 再取队列：enqueue 与 drain 交错时不会丢唤醒。
        kick_pending_.store(false, std::memory_order_release);
        {
            std::lock_guard lock{mutex_};
            while (!outbox_.empty()) {
                write_queue_.push_back(std::move(outbox_.front()));
                outbox_.pop_front();
            }
            outbox_bytes_ = 0;
        }
        if (!writing_) {
            write_next();
        }
    }

    void write_next() {
        if (write_queue_.empty()) {
            writing_ = false;
            bool close_requested;
            {
                std::lock_guard lock{mutex_};
                close_requested = close_requested_;
            }
            if (close_requested) {
                do_close();
            }
            return;
        }
        writing_ = true;
        current_write_ = std::move(write_queue_.front());
        write_queue_.pop_front();
        ws_.binary(true);
        auto self = shared_from_this();
        ws_.async_write(
            asio::buffer(current_write_.data(), current_write_.size()),
            [self](beast::error_code error, std::size_t) { self->on_write(error); });
    }

    void on_write(beast::error_code error) {
        if (error) {
            report_closed();
            return;
        }
        write_next();
    }

    void handle_close_request() {
        if (!writing_) {
            do_close();
        }
    }

    void do_close() {
        if (closing_) {
            return;
        }
        closing_ = true;
        auto self = shared_from_this();
        ws_.async_close(websocket::close_code::normal,
                        [self](beast::error_code) { self->report_closed(); });
    }

    void report_closed() {
        if (closed_reported_) {
            return;
        }
        closed_reported_ = true;
        beast::error_code ignored;
        beast::get_lowest_layer(ws_).socket().close(ignored);
        owner_.push_inbound(inbound_event::closed(connection_, shared_from_this()));
    }

    websocket::stream<beast::tcp_stream> ws_;
    session_owner& owner_;
    projection::ConnectionId connection_;
    shard_context* shard_;
    beast::flat_buffer buffer_;
    // accept 完成后失效；仅存于分片线程使用期间。
    http::request<http::string_body> request_;

    // 跨线程共享状态（mutex_ 保护）：主线程写入，分片线程取出。
    mutable std::mutex mutex_;
    std::deque<FrameBytes> outbox_;
    std::size_t outbox_bytes_{};
    bool close_requested_{};
    std::atomic<bool> kick_pending_{};

    // 仅分片线程访问。
    std::deque<FrameBytes> write_queue_;
    FrameBytes current_write_;
    bool writing_{};
    bool closing_{};
    bool closed_reported_{};
};

struct shard_context {
    shard_context() : work(asio::make_work_guard(io)) {}

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    std::thread thread;
    // 仅分片线程访问；停止并 join 后才可在主线程清空。
    std::unordered_map<projection::ConnectionId, std::shared_ptr<shard_session>,
                       projection::ConnectionIdHash>
        sessions;
};

} // namespace

struct StreamTransport::Impl : session_owner {
    GatewayCore& core_ref;
    protocol::ProtocolLimits wire_limits;
    TransportConfig config;
    asio::io_context io;
    std::optional<tcp::acceptor> acceptor;
    std::optional<asio::ssl::context> tls_context;
    // 主线程本地会话：握手中的连接与 TLS 连接（TLS 不分片，行为不变）。
    std::unordered_map<projection::ConnectionId, std::shared_ptr<ws_session_base>,
                       projection::ConnectionIdHash>
        sessions;
    // 分片连接登记（仅主线程读写）：ConnectionId -> 归属分片上的会话。
    std::unordered_map<projection::ConnectionId, std::shared_ptr<shard_session>,
                       projection::ConnectionIdHash>
        remotes;
    std::vector<std::unique_ptr<shard_context>> shards;
    // 分片线程 -> 主线程事件队列（inbox_mutex 保护）。
    std::mutex inbox_mutex;
    std::deque<inbound_event> inbox;
    std::uint64_t next_connection{1};
    std::size_t next_shard{};
    bool accepting{};

    Impl(GatewayCore& gateway_core, protocol::ProtocolLimits limits,
         TransportConfig transport_config)
        : core_ref(gateway_core), wire_limits(limits),
          config(std::move(transport_config)) {}

    [[nodiscard]] GatewayCore& core() noexcept override { return core_ref; }
    [[nodiscard]] const protocol::ProtocolLimits& limits() const noexcept override {
        return wire_limits;
    }
    [[nodiscard]] bool sharding_enabled() const noexcept override {
        return !shards.empty();
    }

    void push_inbound(inbound_event event) override {
        std::lock_guard lock{inbox_mutex};
        inbox.push_back(std::move(event));
    }

    // 主线程：accept 轮转选择归属分片，socket 原生句柄与升级请求经 post 移交
    // 分片线程；websocket accept 与 attach 事件都在分片线程完成。
    bool handoff_to_shard(
        projection::ConnectionId connection, tcp::socket&& socket,
        http::request<http::string_body>&& request) override {
        if (shards.empty()) {
            return false;
        }
        shard_context& shard = *shards[next_shard++ % shards.size()];
        beast::error_code error;
        const tcp::socket::protocol_type protocol = socket.local_endpoint(error).protocol();
        if (error) {
            return false;
        }
        const tcp::socket::native_handle_type native = socket.release();
        // adopted socket 在主线程构造（注册是加锁/原子路径）：若 post 的 lambda
        // 因分片 io 停止而未能执行，socket 析构会正常关闭句柄，不会泄漏 fd。
        tcp::socket adopted{shard.io, protocol, native};
        asio::post(shard.io,
                   [this, &shard, connection,
                    adopted = std::move(adopted),
                    request = std::move(request)]() mutable {
                       auto session = std::make_shared<shard_session>(
                           std::move(adopted), std::move(request), *this, connection,
                           &shard);
                       shard.sessions.emplace(connection, session);
                       session->start();
                   });
        return true;
    }

    // ---- 主线程 poll 的分阶段处理 ----

    void drain_inbox() {
        std::deque<inbound_event> events;
        {
            std::lock_guard lock{inbox_mutex};
            events.swap(inbox);
        }
        for (inbound_event& event : events) {
            switch (event.kind) {
            case inbound_event::Kind::attach:
                remotes.emplace(event.connection, std::move(event.session));
                break;
            case inbound_event::Kind::frame:
                handle_remote_frame(event);
                break;
            case inbound_event::Kind::closed:
                core().detach_stream(event.connection);
                remotes.erase(event.connection);
                if (event.session != nullptr && event.session->shard() != nullptr) {
                    shard_context& shard = *event.session->shard();
                    const projection::ConnectionId connection = event.connection;
                    asio::post(shard.io,
                               [&shard, connection] { shard.sessions.erase(connection); });
                }
                break;
            }
        }
    }

    void handle_remote_frame(inbound_event& event) {
        protocol::DecodeFailure failure;
        const std::optional<protocol::WireClientControl> control =
            protocol::decode_client_control(
                std::span<const std::uint8_t>{
                    reinterpret_cast<const std::uint8_t*>(event.bytes.data()),
                    event.bytes.size()},
                failure, wire_limits);
        if (!control.has_value()) {
            // 畸形输入：回送稳定错误码协议错误帧后关闭（与本地会话同一语义）。
            if (event.session != nullptr) {
                event.session->enqueue(encode_protocol_error(
                    failure.error_code, failure.message, wire_limits));
                event.session->request_close();
            }
            return;
        }
        if (const auto* ack = std::get_if<protocol::WireAck>(&*control)) {
            static_cast<void>(core().inbound_ack(event.connection, ack->stream_epoch,
                                                 ack->snapshot_id));
        } else {
            static_cast<void>(core().inbound_keyframe_request(event.connection));
        }
    }

    void flush_local() {
        for (auto& [connection, session] : sessions) {
            if (!session->closed() && core().must_disconnect(connection)) {
                session->close();
            }
            while (!session->closed()
                   && session->queued_bytes() < config.max_write_buffer_bytes) {
                std::optional<FrameBytes> frame = core().next_outbound(connection);
                if (!frame.has_value()) {
                    break;
                }
                session->enqueue(std::move(*frame));
            }
        }
    }

    void flush_remote() {
        for (auto& [connection, session] : remotes) {
            if (core().must_disconnect(connection)) {
                session->request_close();
                continue;
            }
            while (session->outbox_bytes() < config.max_write_buffer_bytes) {
                std::optional<FrameBytes> frame = core().next_outbound(connection);
                if (!frame.has_value()) {
                    break;
                }
                session->enqueue(std::move(*frame));
            }
        }
    }

    void sweep_local() {
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (it->second->closed()) {
                // 已移交分片的连接仍活跃，不在此 detach。
                if (!it->second->transferred()) {
                    core().detach_stream(it->first);
                }
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    void accept_next() {
        acceptor->async_accept(io, [this](beast::error_code error, tcp::socket socket) {
            if (error) {
                if (accepting) {
                    accept_next();
                }
                return;
            }
            const projection::ConnectionId connection{next_connection++};
            std::shared_ptr<ws_session_base> session;
            if (tls_context.has_value()) {
                session = std::make_shared<tls_session>(
                    beast::ssl_stream<beast::tcp_stream>{
                        beast::tcp_stream{std::move(socket)}, *tls_context},
                    *this, connection, config.stream_path);
            } else {
                session = std::make_shared<plain_session>(
                    beast::tcp_stream{std::move(socket)}, *this, connection,
                    config.stream_path);
            }
            sessions.emplace(connection, session);
            session->start();
            accept_next();
        });
    }
};

StreamTransport::StreamTransport(GatewayCore& core, protocol::ProtocolLimits limits,
                                 TransportConfig config)
    : impl_(std::make_unique<Impl>(core, limits, std::move(config))) {}

StreamTransport::~StreamTransport() {
    shutdown();
}

[[nodiscard]] bool StreamTransport::start(std::string& diagnostic) {
    Impl& impl = *impl_;
    if (impl.config.io_thread_count == 0) {
        diagnostic = "TransportConfig.io_thread_count 必须 >= 1";
        return false;
    }
    if (!is_loopback_address(impl.config.listen_address) && !impl.config.tls.has_value()) {
        diagnostic = "非 loopback 数据面监听必须配置 TLS 证书";
        return false;
    }

    beast::error_code error;
    if (impl.config.tls.has_value()) {
        impl.tls_context.emplace(asio::ssl::context::tls_server);
        impl.tls_context->use_certificate_chain_file(
            impl.config.tls->certificate_chain_file, error);
        if (error) {
            diagnostic = "TLS 证书链加载失败: " + error.message();
            return false;
        }
        impl.tls_context->use_private_key_file(impl.config.tls->private_key_file,
                                               asio::ssl::context::pem, error);
        if (error) {
            diagnostic = "TLS 私钥加载失败: " + error.message();
            return false;
        }
    }

    const asio::ip::address address =
        asio::ip::make_address(impl.config.listen_address, error);
    if (error) {
        diagnostic = "监听地址非法（需数值地址）: " + impl.config.listen_address;
        return false;
    }
    impl.acceptor.emplace(impl.io);
    impl.acceptor->open(address.is_v4() ? tcp::v4() : tcp::v6(), error);
    if (!error) {
        impl.acceptor->set_option(asio::socket_base::reuse_address{true}, error);
    }
    if (!error) {
        impl.acceptor->bind({address, impl.config.port}, error);
    }
    if (!error) {
        impl.acceptor->listen(asio::socket_base::max_listen_connections, error);
    }
    if (error) {
        diagnostic = "数据面监听失败: " + error.message();
        return false;
    }
    // 分片线程先于 accept 启动；每分片一个 io_context + 专属线程 + work guard。
    if (impl.config.io_thread_count > 1) {
        impl.shards.reserve(impl.config.io_thread_count);
        for (std::uint32_t index = 0; index < impl.config.io_thread_count; ++index) {
            auto shard = std::make_unique<shard_context>();
            shard->thread = std::thread{[&shard_io = shard->io] { shard_io.run(); }};
            impl.shards.push_back(std::move(shard));
        }
    }
    impl.accepting = true;
    impl.accept_next();
    diagnostic.clear();
    return true;
}

void StreamTransport::poll() {
    Impl& impl = *impl_;
    // 主线程：驱动本地 accept/握手/TLS 回调，随后处理分片事件、flush 出站并清扫。
    static_cast<void>(impl.io.poll());
    impl.io.restart();

    impl.drain_inbox();
    impl.flush_local();
    impl.flush_remote();
    impl.sweep_local();
    // 让本次 flush/close 启动的异步操作尽快推进。
    static_cast<void>(impl.io.poll());
    impl.io.restart();
}

void StreamTransport::shutdown() {
    Impl& impl = *impl_;
    impl.accepting = false;
    beast::error_code ignored;
    if (impl.acceptor.has_value()) {
        impl.acceptor->close(ignored);
    }
    for (auto& [connection, session] : impl.sessions) {
        session->close();
    }
    for (auto& [connection, session] : impl.remotes) {
        session->request_close();
    }
    static_cast<void>(impl.io.poll());
    // 分片线程 join 前先停 io_context：剩余 handler 随 io 析构清空，
    // handler 持有的 shared_ptr 保证会话对象在 join 前不被销毁。
    for (auto& shard : impl.shards) {
        shard->work.reset();
        shard->io.stop();
    }
    for (auto& shard : impl.shards) {
        if (shard->thread.joinable()) {
            shard->thread.join();
        }
        shard->sessions.clear();
    }
    // join 后不会再有新事件；清空残余事件，使其持有的 shard_session 引用在
    // 分片 io_context 析构前释放（ws 流析构要访问归属 io 的 beast service）。
    impl.drain_inbox();
    {
        std::lock_guard lock{impl.inbox_mutex};
        impl.inbox.clear();
    }
    for (auto& [connection, session] : impl.sessions) {
        impl.core().detach_stream(connection);
    }
    for (auto& [connection, session] : impl.remotes) {
        impl.core().detach_stream(connection);
    }
    impl.sessions.clear();
    impl.remotes.clear();
    impl.shards.clear();
    impl.io.stop();
}

[[nodiscard]] std::vector<projection::ConnectionId> StreamTransport::active_connections()
    const {
    std::vector<projection::ConnectionId> connections;
    connections.reserve(impl_->sessions.size() + impl_->remotes.size());
    for (const auto& [connection, session] : impl_->sessions) {
        if (!session->closed()) {
            connections.push_back(connection);
        }
    }
    for (const auto& [connection, session] : impl_->remotes) {
        connections.push_back(connection);
    }
    return connections;
}

[[nodiscard]] std::uint16_t StreamTransport::bound_port() const noexcept {
    if (!impl_->acceptor.has_value()) {
        return 0;
    }
    beast::error_code error;
    const tcp::endpoint endpoint = impl_->acceptor->local_endpoint(error);
    return error ? 0 : endpoint.port();
}

} // namespace geoworld::gateway
