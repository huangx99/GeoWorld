#include "stream_client.hpp"

#include "geoworld/gateway/control_params.hpp"
#include "geoworld/protocol/codec.hpp"
#include "geoworld/protocol/version.hpp"

#include "geoworld_control.grpc.pb.h"

#include <grpcpp/create_channel.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <fstream>
#include <sstream>
#include <utility>

namespace geoworld::client {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace pb = geoworld::control::v1;
using tcp = asio::ip::tcp;

void set_pb_value(pb::PropertyValue* target, const world::PropertyValue& value) {
    std::visit(
        [target](const auto& inner) {
            using T = std::decay_t<decltype(inner)>;
            if constexpr (std::is_same_v<T, std::int64_t>) {
                target->set_int64_value(inner);
            } else if constexpr (std::is_same_v<T, double>) {
                target->set_double_value(inner);
            } else if constexpr (std::is_same_v<T, bool>) {
                target->set_bool_value(inner);
            } else {
                target->set_string_value(inner);
            }
        },
        value);
}

[[nodiscard]] std::string read_file(const std::string& path) {
    std::ifstream stream{path, std::ios::binary};
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

} // namespace

struct StreamClient::Impl {
    StreamClientConfig config;
    ClientSessionInfo session;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<pb::GeoWorldControl::Stub> stub;

    asio::io_context io;
    std::optional<asio::ssl::context> tls_context;
    std::optional<websocket::stream<beast::tcp_stream>> plain_ws;
    std::optional<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> tls_ws;
    bool using_tls{};
    bool stream_open{};

    // gRPC ClientContext 不可拷贝/移动，只能在调用点就地构造。
    void apply_deadline(grpc::ClientContext& context) const {
        context.set_deadline(std::chrono::system_clock::now() + config.io_timeout);
    }

    void close_lowest_layer() {
        beast::error_code ignored;
        if (using_tls && tls_ws.has_value()) {
            beast::get_lowest_layer(*tls_ws).socket().close(ignored);
        } else if (plain_ws.has_value()) {
            beast::get_lowest_layer(*plain_ws).socket().close(ignored);
        }
    }

    // 阻塞运行一个异步操作，附带超时看门狗；所有客户端 IO 单线程串行。
    template <typename Starter>
    [[nodiscard]] beast::error_code run(Starter&& starter) {
        beast::error_code result{asio::error::timed_out};
        asio::steady_timer watchdog{io};
        bool done = false;
        watchdog.expires_after(config.io_timeout);
        watchdog.async_wait([this, &done](beast::error_code error) {
            if (!error && !done) {
                close_lowest_layer();
            }
        });
        starter([&result, &done, &watchdog](beast::error_code error, std::size_t = 0) {
            result = error;
            done = true;
            static_cast<void>(watchdog.cancel());
        });
        io.restart();
        io.run();
        return result;
    }

    template <typename Stream>
    [[nodiscard]] beast::error_code ws_write(Stream& ws,
                                             std::span<const std::uint8_t> bytes) {
        ws.binary(true);
        return run([&ws, &bytes](auto handler) {
            ws.async_write(asio::buffer(bytes.data(), bytes.size()),
                           [handler = std::move(handler)](beast::error_code error,
                                                          std::size_t written) mutable {
                               handler(error, written);
                           });
        });
    }
};

StreamClient::StreamClient(StreamClientConfig config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
}

StreamClient::~StreamClient() = default;

[[nodiscard]] bool StreamClient::open_session(std::string& diagnostic) {
    Impl& impl = *impl_;
    std::shared_ptr<grpc::ChannelCredentials> credentials;
    if (impl.config.use_tls) {
        grpc::SslCredentialsOptions options;
        if (!impl.config.tls_root_certificate_file.empty()) {
            options.pem_root_certs = read_file(impl.config.tls_root_certificate_file);
        }
        credentials = grpc::SslCredentials(options);
    } else {
        credentials = grpc::InsecureChannelCredentials();
    }
    impl.channel = grpc::CreateChannel(impl.config.control_address, credentials);
    impl.stub = pb::GeoWorldControl::NewStub(impl.channel);

    pb::OpenSessionRequest request;
    request.set_credential_token(impl.config.credential_token);
    request.mutable_control_versions()->set_minimum(protocol::control_api_version);
    request.mutable_control_versions()->set_maximum(protocol::control_api_version);
    request.mutable_data_versions()->set_minimum(protocol::data_schema_version);
    request.mutable_data_versions()->set_maximum(protocol::data_schema_version);
    request.set_client_description("gw-stream-client");

    grpc::ClientContext context;
    impl.apply_deadline(context);
    pb::OpenSessionResponse response;
    const grpc::Status status = impl.stub->OpenSession(&context, request, &response);
    if (!status.ok()) {
        diagnostic = "OpenSession RPC 失败: " + status.error_message();
        return false;
    }
    if (!response.error().code().empty()) {
        diagnostic = "OpenSession 拒绝: " + response.error().code();
        return false;
    }
    impl.session.session_id = response.session_id();
    impl.session.stream_ticket = response.stream_ticket();
    impl.session.data_endpoint = response.data_endpoint();
    impl.session.current_tick = response.current_tick();
    impl.session.control_api_version = response.control_api_version();
    impl.session.data_schema_version = response.data_schema_version();
    diagnostic.clear();
    return true;
}

[[nodiscard]] bool StreamClient::update_subscription(
    const projection::Subscription& subscription, std::string& diagnostic) {
    Impl& impl = *impl_;
    pb::UpdateSubscriptionRequest request;
    request.set_session_id(impl.session.session_id);
    if (subscription.area.has_value()) {
        pb::AabbEnu* area = request.mutable_subscription()->mutable_area();
        area->mutable_minimum()->set_east(subscription.area->minimum.east);
        area->mutable_minimum()->set_north(subscription.area->minimum.north);
        area->mutable_minimum()->set_up(subscription.area->minimum.up);
        area->mutable_maximum()->set_east(subscription.area->maximum.east);
        area->mutable_maximum()->set_north(subscription.area->maximum.north);
        area->mutable_maximum()->set_up(subscription.area->maximum.up);
    } else if (subscription.follow.has_value()) {
        pb::FollowSubscription* follow =
            request.mutable_subscription()->mutable_follow();
        follow->set_follow_wid(subscription.follow->value);
        follow->set_radius_meters(subscription.follow_radius_meters);
    } else {
        diagnostic = "订阅必须包含 AOI 或跟随目标";
        return false;
    }

    grpc::ClientContext context;
    impl.apply_deadline(context);
    pb::UpdateSubscriptionResponse response;
    const grpc::Status status =
        impl.stub->UpdateSubscription(&context, request, &response);
    if (!status.ok()) {
        diagnostic = "UpdateSubscription RPC 失败: " + status.error_message();
        return false;
    }
    if (!response.updated()) {
        diagnostic = "UpdateSubscription 拒绝: " + response.error().code();
        return false;
    }
    diagnostic.clear();
    return true;
}

[[nodiscard]] bool StreamClient::acquire_ownership(
    foundation::WorldId target, const std::vector<std::string>& keys,
    std::uint64_t lease_until_tick, std::string& diagnostic) {
    Impl& impl = *impl_;
    pb::AcquireOwnershipRequest request;
    request.set_session_id(impl.session.session_id);
    request.set_target_wid(target.value);
    for (const std::string& key : keys) {
        request.add_property_keys(key);
    }
    request.set_lease_until_tick(lease_until_tick);

    grpc::ClientContext context;
    impl.apply_deadline(context);
    pb::AcquireOwnershipResponse response;
    const grpc::Status status =
        impl.stub->AcquireOwnership(&context, request, &response);
    if (!status.ok()) {
        diagnostic = "AcquireOwnership RPC 失败: " + status.error_message();
        return false;
    }
    if (!response.granted()) {
        diagnostic = "AcquireOwnership 拒绝: " + response.error().code();
        return false;
    }
    diagnostic.clear();
    return true;
}

[[nodiscard]] bool StreamClient::release_ownership(
    foundation::WorldId target, const std::vector<std::string>& keys,
    std::string& diagnostic) {
    Impl& impl = *impl_;
    pb::ReleaseOwnershipRequest request;
    request.set_session_id(impl.session.session_id);
    request.set_target_wid(target.value);
    for (const std::string& key : keys) {
        request.add_property_keys(key);
    }

    grpc::ClientContext context;
    impl.apply_deadline(context);
    pb::ReleaseOwnershipResponse response;
    const grpc::Status status =
        impl.stub->ReleaseOwnership(&context, request, &response);
    if (!status.ok()) {
        diagnostic = "ReleaseOwnership RPC 失败: " + status.error_message();
        return false;
    }
    diagnostic.clear();
    return response.released();
}

[[nodiscard]] std::optional<SubmitResult>
StreamClient::submit_set_property(foundation::WorldId target, const std::string& key,
                                  const world::PropertyValue& value,
                                  std::uint64_t client_sequence,
                                  std::uint64_t expected_object_version,
                                  std::string& diagnostic) {
    Impl& impl = *impl_;
    pb::SubmitCommandRequest request;
    request.set_session_id(impl.session.session_id);
    request.set_client_sequence(client_sequence);
    request.set_target_wid(target.value);
    request.set_operation(pb::SET_PROPERTY);
    // SET_PROPERTY 约定：parameters 恰好一个条目，键即属性键，值即属性值。
    set_pb_value(&(*request.mutable_parameters())[key], value);
    request.set_expected_object_version(expected_object_version);

    grpc::ClientContext context;
    impl.apply_deadline(context);
    pb::SubmitCommandResponse response;
    const grpc::Status status = impl.stub->SubmitCommand(&context, request, &response);
    if (!status.ok()) {
        diagnostic = "SubmitCommand RPC 失败: " + status.error_message();
        return std::nullopt;
    }
    SubmitResult result;
    result.ingress_sequence = response.ingress_sequence();
    result.error_code = response.error().code();
    switch (response.status()) {
    case pb::ACCEPTED:
        result.status = gateway::ReceiptStatus::accepted;
        break;
    case pb::APPLIED:
        result.status = gateway::ReceiptStatus::applied;
        break;
    case pb::DUPLICATE:
        result.status = gateway::ReceiptStatus::duplicate;
        break;
    default:
        result.status = gateway::ReceiptStatus::rejected;
        break;
    }
    diagnostic.clear();
    return result;
}

[[nodiscard]] bool StreamClient::request_keyframe(std::string& diagnostic) {
    Impl& impl = *impl_;
    pb::RequestKeyframeRequest request;
    request.set_session_id(impl.session.session_id);
    grpc::ClientContext context;
    impl.apply_deadline(context);
    pb::RequestKeyframeResponse response;
    const grpc::Status status = impl.stub->RequestKeyframe(&context, request, &response);
    if (!status.ok() || !response.scheduled()) {
        diagnostic = status.ok() ? "RequestKeyframe 拒绝" : status.error_message();
        return false;
    }
    diagnostic.clear();
    return true;
}

[[nodiscard]] bool StreamClient::close_session(std::string& diagnostic) {
    Impl& impl = *impl_;
    pb::CloseSessionRequest request;
    request.set_session_id(impl.session.session_id);
    grpc::ClientContext context;
    impl.apply_deadline(context);
    pb::CloseSessionResponse response;
    const grpc::Status status = impl.stub->CloseSession(&context, request, &response);
    if (!status.ok()) {
        diagnostic = "CloseSession RPC 失败: " + status.error_message();
        return false;
    }
    diagnostic.clear();
    return response.closed();
}

[[nodiscard]] bool StreamClient::connect_stream(std::string& diagnostic,
                                                std::string_view ticket) {
    Impl& impl = *impl_;
    const std::string_view effective_ticket =
        ticket.empty() ? std::string_view{impl.session.stream_ticket} : ticket;
    if (effective_ticket.empty()) {
        diagnostic = "缺少 stream ticket，先 open_session";
        return false;
    }
    const std::string target = impl.config.stream_path + "?"
        + std::string(gateway::stream_ticket_query_key) + "="
        + std::string{effective_ticket};
    const std::string host_header =
        impl.config.stream_host + ":" + std::to_string(impl.config.stream_port);

    beast::error_code error;
    impl.using_tls = impl.config.use_tls;
    if (impl.using_tls) {
        impl.tls_context.emplace(asio::ssl::context::tls_client);
        if (!impl.config.tls_root_certificate_file.empty()) {
            impl.tls_context->load_verify_file(impl.config.tls_root_certificate_file,
                                               error);
            if (error) {
                diagnostic = "根证书加载失败: " + error.message();
                return false;
            }
            impl.tls_context->set_verify_mode(asio::ssl::verify_peer);
        } else {
            // 未提供根证书时不校验对端，仅限 loopback 测试。
            impl.tls_context->set_verify_mode(asio::ssl::verify_none);
        }
        tcp::resolver resolver{impl.io};
        const auto endpoints =
            resolver.resolve(impl.config.stream_host,
                             std::to_string(impl.config.stream_port), error);
        if (error) {
            diagnostic = "地址解析失败: " + error.message();
            return false;
        }
        impl.tls_ws.emplace(beast::tcp_stream{impl.io}, *impl.tls_context);
        error = impl.run([&](auto handler) {
            beast::get_lowest_layer(*impl.tls_ws).async_connect(
                endpoints, [handler = std::move(handler)](beast::error_code connect_error,
                                                          const tcp::endpoint&) mutable {
                    handler(connect_error, 0);
                });
        });
        if (!error) {
            if (!SSL_set_tlsext_host_name(
                    impl.tls_ws->next_layer().native_handle(),
                    impl.config.stream_host.c_str())) {
                error = beast::error_code{static_cast<int>(::ERR_get_error()),
                                          asio::error::get_ssl_category()};
            }
        }
        if (!error) {
            error = impl.run([&](auto handler) {
                impl.tls_ws->next_layer().async_handshake(
                    asio::ssl::stream_base::client, std::move(handler));
            });
        }
        if (error) {
            diagnostic = "TLS 连接失败: " + error.message();
            return false;
        }
        auto& ws = *impl.tls_ws;
        ws.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& request) {
                request.set(boost::beast::http::field::sec_websocket_protocol,
                            protocol::websocket_subprotocol);
            }));
        error = impl.run([&](auto handler) {
            ws.async_handshake(host_header, target, std::move(handler));
        });
    } else {
        tcp::resolver resolver{impl.io};
        const auto endpoints =
            resolver.resolve(impl.config.stream_host,
                             std::to_string(impl.config.stream_port), error);
        if (error) {
            diagnostic = "地址解析失败: " + error.message();
            return false;
        }
        impl.plain_ws.emplace(beast::tcp_stream{impl.io});
        error = impl.run([&](auto handler) {
            beast::get_lowest_layer(*impl.plain_ws).async_connect(
                endpoints, [handler = std::move(handler)](beast::error_code connect_error,
                                                          const tcp::endpoint&) mutable {
                    handler(connect_error, 0);
                });
        });
        if (error) {
            diagnostic = "数据面连接失败: " + error.message();
            return false;
        }
        auto& ws = *impl.plain_ws;
        ws.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& request) {
                request.set(boost::beast::http::field::sec_websocket_protocol,
                            protocol::websocket_subprotocol);
            }));
        error = impl.run([&](auto handler) {
            ws.async_handshake(host_header, target, std::move(handler));
        });
    }
    if (error) {
        diagnostic = "WebSocket 握手失败（ticket 无效或已使用）: " + error.message();
        return false;
    }
    impl.stream_open = true;
    diagnostic.clear();
    return true;
}

[[nodiscard]] std::optional<protocol::WireFrame> StreamClient::read_frame(
    std::string& diagnostic) {
    Impl& impl = *impl_;
    if (!impl.stream_open) {
        diagnostic = "数据面未连接";
        return std::nullopt;
    }
    auto buffer = std::make_shared<beast::flat_buffer>();
    beast::error_code error;
    if (impl.using_tls) {
        error = impl.run([&](auto handler) {
            impl.tls_ws->async_read(*buffer, std::move(handler));
        });
    } else {
        error = impl.run([&](auto handler) {
            impl.plain_ws->async_read(*buffer, std::move(handler));
        });
    }
    if (error) {
        diagnostic = "数据面读取失败: " + error.message();
        impl.stream_open = false;
        return std::nullopt;
    }
    // flat_buffer::data() 返回单段 const_buffer，直接取裸指针与长度。
    const auto* bytes = static_cast<const std::uint8_t*>(buffer->data().data());
    protocol::DecodeFailure failure;
    std::optional<protocol::WireFrame> frame = protocol::decode_server_frame(
        std::span<const std::uint8_t>{bytes, buffer->size()}, failure);
    if (!frame.has_value()) {
        diagnostic = "服务端帧解码失败: " + std::string(failure.error_code);
        return std::nullopt;
    }
    diagnostic.clear();
    return frame;
}

[[nodiscard]] bool StreamClient::send_ack(std::uint64_t stream_epoch,
                                          std::uint64_t snapshot_id,
                                          std::string& diagnostic) {
    Impl& impl = *impl_;
    const std::vector<std::uint8_t> bytes = protocol::encode_client_control(
        protocol::WireClientControl{protocol::WireAck{stream_epoch, snapshot_id}});
    const beast::error_code error =
        impl.using_tls ? impl.ws_write(*impl.tls_ws, bytes)
                       : impl.ws_write(*impl.plain_ws, bytes);
    if (error) {
        diagnostic = "ack 发送失败: " + error.message();
        return false;
    }
    diagnostic.clear();
    return true;
}

[[nodiscard]] bool StreamClient::send_keyframe_request(std::string_view reason,
                                                       std::string& diagnostic) {
    Impl& impl = *impl_;
    const std::vector<std::uint8_t> bytes = protocol::encode_client_control(
        protocol::WireClientControl{
            protocol::WireKeyframeRequest{std::string{reason}}});
    const beast::error_code error =
        impl.using_tls ? impl.ws_write(*impl.tls_ws, bytes)
                       : impl.ws_write(*impl.plain_ws, bytes);
    if (error) {
        diagnostic = "keyframe 请求发送失败: " + error.message();
        return false;
    }
    diagnostic.clear();
    return true;
}

[[nodiscard]] bool StreamClient::send_raw_bytes(std::span<const std::uint8_t> bytes,
                                                std::string& diagnostic) {
    Impl& impl = *impl_;
    if (!impl.stream_open) {
        diagnostic = "数据面未连接";
        return false;
    }
    const beast::error_code error =
        impl.using_tls ? impl.ws_write(*impl.tls_ws, bytes)
                       : impl.ws_write(*impl.plain_ws, bytes);
    if (error) {
        // 服务器可能在读完整帧前已关闭连接（如超大帧），写失败不由本函数判错。
        diagnostic = "原始帧发送失败: " + error.message();
        return false;
    }
    diagnostic.clear();
    return true;
}

void StreamClient::disconnect_stream() {
    Impl& impl = *impl_;
    if (!impl.stream_open) {
        return;
    }
    impl.stream_open = false;
    if (impl.using_tls) {
        static_cast<void>(impl.run([&](auto handler) {
            impl.tls_ws->async_close(websocket::close_code::normal,
                                     std::move(handler));
        }));
    } else {
        static_cast<void>(impl.run([&](auto handler) {
            impl.plain_ws->async_close(websocket::close_code::normal,
                                       std::move(handler));
        }));
    }
    impl.close_lowest_layer();
}

[[nodiscard]] const ClientSessionInfo& StreamClient::session() const noexcept {
    return impl_->session;
}

} // namespace geoworld::client
