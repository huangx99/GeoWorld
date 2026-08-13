#include "thin_client.hpp"

#include "geoworld/protocol/codec.hpp"
#include "geoworld/protocol/version.hpp"
#include "geoworld/protocol/wire.hpp"

#include "world_stream_generated.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket/stream.hpp>

#include <deque>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace geoworld::client {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace stream_fb = geoworld::stream::v1;
using tcp = asio::ip::tcp;

// 最便宜的服务端帧解析：只读 root 表 union 类型与状态帧的 epoch/snapshot 两个
// 标量字段；file identifier 是常数memcmp，不做 Verifier 全遍历。
enum class InspectResult { keyframe, delta, heartbeat, reliable, invalid };

[[nodiscard]] InspectResult inspect_server_frame(const std::uint8_t* data,
                                                 std::size_t size,
                                                 std::uint64_t& stream_epoch,
                                                 std::uint64_t& snapshot_id) {
    // file identifier 紧跟 root 偏移：长度下限保证两次读不越界。
    constexpr std::size_t kMinFrameBytes =
        sizeof(flatbuffers::uoffset_t) + 4U;
    if (size < kMinFrameBytes
        || !flatbuffers::BufferHasIdentifier(data, "GWSF")) {
        return InspectResult::invalid;
    }
    const stream_fb::WorldStreamFrame* root = stream_fb::GetWorldStreamFrame(data);
    switch (root->frame_type()) {
    case stream_fb::ServerFrame::KeyframeFrame: {
        const stream_fb::KeyframeFrame* frame = root->frame_as_KeyframeFrame();
        if (frame == nullptr) {
            return InspectResult::invalid;
        }
        stream_epoch = frame->stream_epoch();
        snapshot_id = frame->snapshot_id();
        return InspectResult::keyframe;
    }
    case stream_fb::ServerFrame::DeltaFrame: {
        const stream_fb::DeltaFrame* frame = root->frame_as_DeltaFrame();
        if (frame == nullptr) {
            return InspectResult::invalid;
        }
        stream_epoch = frame->stream_epoch();
        snapshot_id = frame->snapshot_id();
        return InspectResult::delta;
    }
    case stream_fb::ServerFrame::HeartbeatFrame:
        return InspectResult::heartbeat;
    case stream_fb::ServerFrame::ReliableEvent:
        return InspectResult::reliable;
    default:
        return InspectResult::invalid;
    }
}

// 单连接异步状态机：除显式标注外全部字段只在所属 io_context 线程触碰。
class ThinConnection : public std::enable_shared_from_this<ThinConnection> {
public:
    ThinConnection(asio::io_context& io, const ThinClientPoolConfig& config,
                   ThinConnectionStats& stats, std::atomic<bool>& stopping)
        : resolver_{io}, ws_{beast::tcp_stream{io}}, config_{config}, stats_{stats},
          stopping_{stopping} {
        ws_.binary(true);
        ws_.read_message_max(config_.max_frame_bytes);
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& request) {
                request.set(boost::beast::http::field::sec_websocket_protocol,
                            protocol::websocket_subprotocol);
            }));
    }

    // 主线程调用：投递到所属 io 线程执行。
    void begin(std::string ticket) {
        const std::shared_ptr<ThinConnection> self = shared_from_this();
        asio::post(resolver_.get_executor(),
                   [self, ticket = std::move(ticket)]() mutable {
                       self->do_resolve(std::move(ticket));
                   });
    }

    // 主线程调用：原子置位即可，读完成回调在重发前检查。
    void pause_reads() { paused_.store(true, std::memory_order_release); }

    // 主线程调用：恢复置位并投递一个 kick，若连接空闲则补发 async_read。
    void resume_reads() {
        paused_.store(false, std::memory_order_release);
        const std::shared_ptr<ThinConnection> self = shared_from_this();
        asio::post(resolver_.get_executor(), [self] { self->maybe_read(); });
    }

    // 所属 io 线程调用（shutdown 时经 post 进入）：取消未决 IO。
    void close_now() {
        beast::error_code ignored;
        beast::get_lowest_layer(ws_).socket().close(ignored);
    }

    // 主线程调用：投递 close_now 到所属 io 线程。
    void post_close() {
        const std::shared_ptr<ThinConnection> self = shared_from_this();
        asio::post(resolver_.get_executor(), [self] { self->close_now(); });
    }

private:
    void do_resolve(const std::string& ticket) {
        const std::shared_ptr<ThinConnection> self = shared_from_this();
        resolver_.async_resolve(
            config_.host, std::to_string(config_.port),
            [self, ticket](beast::error_code error, tcp::resolver::results_type results) {
                if (error) {
                    self->stats_.failed.store(true, std::memory_order_release);
                    return;
                }
                self->do_connect(std::move(results), ticket);
            });
    }

    void do_connect(tcp::resolver::results_type results, const std::string& ticket) {
        beast::get_lowest_layer(ws_).expires_after(config_.connect_timeout);
        const std::shared_ptr<ThinConnection> self = shared_from_this();
        beast::get_lowest_layer(ws_).async_connect(
            std::move(results),
            [self, ticket](beast::error_code error, const tcp::endpoint&) {
                if (error) {
                    self->stats_.failed.store(true, std::memory_order_release);
                    return;
                }
                self->do_handshake(ticket);
            });
    }

    void do_handshake(const std::string& ticket) {
        const std::string host_header =
            config_.host + ":" + std::to_string(config_.port);
        const std::string target = config_.path + "?"
            + std::string(gateway::stream_ticket_query_key) + "=" + ticket;
        const std::shared_ptr<ThinConnection> self = shared_from_this();
        ws_.async_handshake(host_header, target,
                            [self](beast::error_code error) {
                                if (error) {
                                    self->stats_.failed.store(true,
                                                              std::memory_order_release);
                                    return;
                                }
                                beast::get_lowest_layer(self->ws_).expires_never();
                                self->stats_.established.store(
                                    true, std::memory_order_release);
                                self->maybe_read();
                            });
    }

    // 只在所属 io 线程调用；paused/closed/read 在飞时不重发。
    void maybe_read() {
        if (paused_.load(std::memory_order_acquire) || read_in_flight_
            || stats_.closed.load(std::memory_order_acquire)) {
            return;
        }
        read_in_flight_ = true;
        const std::shared_ptr<ThinConnection> self = shared_from_this();
        ws_.async_read(buffer_,
                       [self](beast::error_code error, std::size_t bytes) {
                           self->on_read(error, bytes);
                       });
    }

    void on_read(beast::error_code error, std::size_t /*bytes*/) {
        read_in_flight_ = false;
        if (error) {
            mark_closed();
            return;
        }
        ++stats_.frames;
        const auto* data = static_cast<const std::uint8_t*>(buffer_.data().data());
        std::uint64_t stream_epoch = 0;
        std::uint64_t snapshot_id = 0;
        const InspectResult result =
            inspect_server_frame(data, buffer_.size(), stream_epoch, snapshot_id);
        buffer_.consume(buffer_.size());
        switch (result) {
        case InspectResult::keyframe:
            ++stats_.keyframes;
            enqueue_ack(stream_epoch, snapshot_id);
            break;
        case InspectResult::delta:
            ++stats_.deltas;
            enqueue_ack(stream_epoch, snapshot_id);
            break;
        case InspectResult::heartbeat:
            ++stats_.heartbeats;
            break;
        case InspectResult::reliable:
            ++stats_.reliable_events;
            break;
        case InspectResult::invalid:
            ++stats_.parse_failures;
            // 与完整客户端解码失败即断开的行为一致。
            stats_.closed.store(true, std::memory_order_release);
            close_now();
            return;
        }
        maybe_read();
    }

    void enqueue_ack(std::uint64_t stream_epoch, std::uint64_t snapshot_id) {
        write_queue_.push_back(protocol::encode_client_control(
            protocol::WireClientControl{protocol::WireAck{stream_epoch, snapshot_id}}));
        if (write_in_flight_) {
            return;
        }
        write_in_flight_ = true;
        const std::shared_ptr<ThinConnection> self = shared_from_this();
        ws_.async_write(asio::buffer(write_queue_.front()),
                        [self](beast::error_code error, std::size_t written) {
                            self->on_write(error, written);
                        });
    }

    void on_write(beast::error_code error, std::size_t /*bytes*/) {
        if (error) {
            mark_closed();
            return;
        }
        ++stats_.acks_sent;
        write_queue_.pop_front();
        if (write_queue_.empty()) {
            write_in_flight_ = false;
            return;
        }
        const std::shared_ptr<ThinConnection> self = shared_from_this();
        ws_.async_write(asio::buffer(write_queue_.front()),
                        [self](beast::error_code write_error, std::size_t written) {
                            self->on_write(write_error, written);
                        });
    }

    // 收尾阶段（set_stopping 之后）的对端关闭不计入客户端侧断开。
    void mark_closed() {
        if (!stopping_.load(std::memory_order_acquire)) {
            stats_.closed.store(true, std::memory_order_release);
        }
    }

    tcp::resolver resolver_;
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    std::deque<std::vector<std::uint8_t>> write_queue_;
    bool read_in_flight_{};
    bool write_in_flight_{};
    ThinClientPoolConfig config_;
    ThinConnectionStats& stats_;
    std::atomic<bool>& stopping_;
    std::atomic<bool> paused_{};
};

} // namespace

struct ThinClientPool::Impl {
    explicit Impl(ThinClientPoolConfig pool_config, std::size_t connection_count)
        : config{std::move(pool_config)} {
        const std::size_t thread_count = std::max<std::size_t>(1, config.io_threads);
        ios.reserve(thread_count);
        guards.reserve(thread_count);
        for (std::size_t index = 0; index < thread_count; ++index) {
            ios.push_back(std::make_unique<asio::io_context>());
            // 工作守卫从创建起挂住：start() 之前 io 线程不会因暂无一投递而退出。
            guards.emplace_back(ios.back()->get_executor());
        }
        stats.reserve(connection_count);
        connections.reserve(connection_count);
        for (std::size_t index = 0; index < connection_count; ++index) {
            stats.push_back(std::make_unique<ThinConnectionStats>());
            connections.push_back(std::make_shared<ThinConnection>(
                *ios[index % thread_count], config, *stats.back(), stopping));
        }
    }

    ThinClientPoolConfig config;
    std::atomic<bool> stopping{false};
    std::vector<std::unique_ptr<asio::io_context>> ios;
    std::vector<asio::executor_work_guard<asio::io_context::executor_type>> guards;
    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<ThinConnectionStats>> stats;
    std::vector<std::shared_ptr<ThinConnection>> connections;
};

ThinClientPool::ThinClientPool(ThinClientPoolConfig config,
                               std::size_t connection_count)
    : impl_(std::make_unique<Impl>(std::move(config), connection_count)) {}

ThinClientPool::~ThinClientPool() {
    shutdown();
}

void ThinClientPool::start() {
    Impl& impl = *impl_;
    if (!impl.threads.empty()) {
        return;
    }
    impl.threads.reserve(impl.ios.size());
    for (const auto& io : impl.ios) {
        impl.threads.emplace_back([&io] { io->run(); });
    }
}

void ThinClientPool::async_connect(std::size_t index, std::string ticket) {
    impl_->connections.at(index)->begin(std::move(ticket));
}

void ThinClientPool::pause_reads(std::size_t index) {
    impl_->connections.at(index)->pause_reads();
}

void ThinClientPool::resume_reads(std::size_t index) {
    impl_->connections.at(index)->resume_reads();
}

[[nodiscard]] const ThinConnectionStats& ThinClientPool::stats(
    std::size_t index) const {
    return *impl_->stats.at(index);
}

void ThinClientPool::set_stopping() {
    impl_->stopping.store(true, std::memory_order_release);
}

void ThinClientPool::shutdown() {
    Impl& impl = *impl_;
    set_stopping();
    for (const std::shared_ptr<ThinConnection>& connection : impl.connections) {
        connection->post_close();
    }
    for (auto& guard : impl.guards) {
        guard.reset();
    }
    for (const auto& io : impl.ios) {
        io->stop();
    }
    for (std::thread& thread : impl.threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    impl.threads.clear();
}

} // namespace geoworld::client
