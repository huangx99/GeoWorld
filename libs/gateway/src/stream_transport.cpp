#include "geoworld/gateway/stream_transport.hpp"

#include "geoworld/gateway/control_params.hpp"
#include "geoworld/gateway/errors.hpp"
#include "geoworld/gateway/frame_codec.hpp"
#include "geoworld/protocol/codec.hpp"
#include "geoworld/protocol/error.hpp"
#include "geoworld/protocol/version.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
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

#include <chrono>
#include <deque>
#include <optional>
#include <string_view>
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

class ws_session_base {
public:
    virtual ~ws_session_base() = default;

    virtual void start() = 0;
    virtual void close() = 0;
    virtual void enqueue(FrameBytes bytes) = 0;
    [[nodiscard]] virtual bool closed() const noexcept = 0;
    [[nodiscard]] virtual std::size_t queued_bytes() const noexcept = 0;
};

struct session_owner {
    virtual ~session_owner() = default;

    [[nodiscard]] virtual GatewayCore& core() noexcept = 0;
    [[nodiscard]] virtual const protocol::ProtocolLimits& limits() const noexcept = 0;
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
        beast::get_lowest_layer(ws_).expires_never();
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& response) {
                response.set(http::field::sec_websocket_protocol,
                             protocol::websocket_subprotocol);
            }));
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
};

using plain_session = ws_session<beast::tcp_stream>;
using tls_session = ws_session<beast::ssl_stream<beast::tcp_stream>>;

} // namespace

struct StreamTransport::Impl : session_owner {
    GatewayCore& core_ref;
    protocol::ProtocolLimits wire_limits;
    TransportConfig config;
    asio::io_context io;
    std::optional<tcp::acceptor> acceptor;
    std::optional<asio::ssl::context> tls_context;
    std::unordered_map<projection::ConnectionId, std::shared_ptr<ws_session_base>,
                       projection::ConnectionIdHash>
        sessions;
    std::uint64_t next_connection{1};
    bool accepting{};

    Impl(GatewayCore& gateway_core, protocol::ProtocolLimits limits,
         TransportConfig transport_config)
        : core_ref(gateway_core), wire_limits(limits),
          config(std::move(transport_config)) {}

    [[nodiscard]] GatewayCore& core() noexcept override { return core_ref; }
    [[nodiscard]] const protocol::ProtocolLimits& limits() const noexcept override {
        return wire_limits;
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
    impl.accepting = true;
    impl.accept_next();
    diagnostic.clear();
    return true;
}

void StreamTransport::poll() {
    Impl& impl = *impl_;
    // 单线程：所有 asio 回调在本调用内执行，随后 flush 出站并清扫关闭的连接。
    static_cast<void>(impl.io.poll());
    impl.io.restart();

    for (auto& [connection, session] : impl.sessions) {
        if (!session->closed() && impl.core().must_disconnect(connection)) {
            session->close();
        }
        while (!session->closed()
               && session->queued_bytes() < impl.config.max_write_buffer_bytes) {
            std::optional<FrameBytes> frame = impl.core().next_outbound(connection);
            if (!frame.has_value()) {
                break;
            }
            session->enqueue(std::move(*frame));
        }
    }

    for (auto it = impl.sessions.begin(); it != impl.sessions.end();) {
        if (it->second->closed()) {
            impl.core().detach_stream(it->first);
            it = impl.sessions.erase(it);
        } else {
            ++it;
        }
    }
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
    static_cast<void>(impl.io.poll());
    for (auto& [connection, session] : impl.sessions) {
        impl.core().detach_stream(connection);
    }
    impl.sessions.clear();
    impl.io.stop();
}

[[nodiscard]] std::vector<projection::ConnectionId> StreamTransport::active_connections()
    const {
    std::vector<projection::ConnectionId> connections;
    connections.reserve(impl_->sessions.size());
    for (const auto& [connection, session] : impl_->sessions) {
        if (!session->closed()) {
            connections.push_back(connection);
        }
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
