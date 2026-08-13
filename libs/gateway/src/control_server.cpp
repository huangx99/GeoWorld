#include "geoworld/gateway/control_server.hpp"

#include "geoworld/gateway/control_params.hpp"
#include "geoworld/gateway/errors.hpp"
#include "geoworld/foundation/version.hpp"
#include "geoworld/protocol/version.hpp"

#include "geoworld_control.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>

#include <charconv>
#include <deque>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <string_view>
#include <utility>

namespace geoworld::gateway {
namespace {

namespace pb = geoworld::control::v1;

[[nodiscard]] bool is_loopback_host(std::string_view host) {
    return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

[[nodiscard]] std::string encode_session_id(SessionId id) {
    return std::to_string(id.value);
}

// session_id 以十进制字符串编码；解析必须完整消费且值有效。
[[nodiscard]] std::optional<SessionId> decode_session_id(const std::string& text) {
    SessionId id;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, id.value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !id.valid()) {
        return std::nullopt;
    }
    return id;
}

void fill_error(pb::Error* error, GatewayError gateway_error) {
    error->set_code(error_code(gateway_error));
    error->set_message(error_code(gateway_error));
}

[[nodiscard]] std::optional<world::PropertyValue> to_property_value(
    const pb::PropertyValue& value) {
    switch (value.value_case()) {
    case pb::PropertyValue::kInt64Value:
        return world::PropertyValue{value.int64_value()};
    case pb::PropertyValue::kDoubleValue:
        return world::PropertyValue{value.double_value()};
    case pb::PropertyValue::kBoolValue:
        return world::PropertyValue{value.bool_value()};
    case pb::PropertyValue::kStringValue:
        return world::PropertyValue{value.string_value()};
    case pb::PropertyValue::VALUE_NOT_SET:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<projection::Subscription> to_subscription(
    const pb::Subscription& subscription, GatewayError& error) {
    // M4-C 字段裁剪只支持全局 ProjectionPolicy；按连接的字段白名单拒绝而不是静默忽略。
    if (!subscription.property_keys().empty() || !subscription.state_keys().empty()) {
        error = GatewayError::invalid_request;
        return std::nullopt;
    }
    projection::Subscription result;
    switch (subscription.interest_case()) {
    case pb::Subscription::kArea: {
        const pb::AabbEnu& area = subscription.area();
        result.area = spatial::Aabb{
            spatial::Enu{area.minimum().east(), area.minimum().north(),
                         area.minimum().up()},
            spatial::Enu{area.maximum().east(), area.maximum().north(),
                         area.maximum().up()},
        };
        break;
    }
    case pb::Subscription::kFollow:
        result.follow = foundation::WorldId{subscription.follow().follow_wid()};
        result.follow_radius_meters = subscription.follow().radius_meters();
        break;
    case pb::Subscription::INTEREST_NOT_SET:
        error = GatewayError::invalid_request;
        return std::nullopt;
    }
    error = GatewayError::none;
    return result;
}

[[nodiscard]] std::pair<GatewayError, CommandParams> to_command_params(
    const pb::SubmitCommandRequest& request) {
    namespace params = geoworld::gateway;
    const auto& parameters = request.parameters();
    switch (request.operation()) {
    case pb::SET_PROPERTY: {
        // SET_PROPERTY 约定：parameters 恰好一个条目，键即属性键，值即属性值。
        if (parameters.size() != 1) {
            return {GatewayError::invalid_request, CommandParams{}};
        }
        const auto& entry = *parameters.begin();
        std::optional<world::PropertyValue> value = to_property_value(entry.second);
        if (!value.has_value()) {
            return {GatewayError::invalid_request, CommandParams{}};
        }
        return {GatewayError::none,
                SetPropertyParams{entry.first, std::move(*value)}};
    }
    case pb::CREATE_OBJECT: {
        CreateObjectParams create;
        create.requested_id = foundation::WorldId{request.target_wid()};
        for (const auto& [key, raw] : parameters) {
            std::optional<world::PropertyValue> value = to_property_value(raw);
            if (!value.has_value()) {
                return {GatewayError::invalid_request, CommandParams{}};
            }
            if (key == params::create_param_semantic_type) {
                const auto* text = std::get_if<std::string>(&*value);
                if (text == nullptr) {
                    return {GatewayError::invalid_request, CommandParams{}};
                }
                create.semantic_type = *text;
            } else if (key == params::create_param_geometry_ref) {
                const auto* text = std::get_if<std::string>(&*value);
                if (text == nullptr) {
                    return {GatewayError::invalid_request, CommandParams{}};
                }
                create.geometry_ref = *text;
            } else if (key == params::create_param_position_x
                       || key == params::create_param_position_y
                       || key == params::create_param_position_z) {
                const auto* number = std::get_if<double>(&*value);
                if (number == nullptr) {
                    return {GatewayError::invalid_request, CommandParams{}};
                }
                if (key == params::create_param_position_x) {
                    create.position.x = *number;
                } else if (key == params::create_param_position_y) {
                    create.position.y = *number;
                } else {
                    create.position.z = *number;
                }
            } else {
                create.properties.emplace(key, std::move(*value));
            }
        }
        return {GatewayError::none, std::move(create)};
    }
    case pb::DESTROY_OBJECT:
        return {GatewayError::none, DestroyObjectParams{}};
    case pb::COMMAND_OPERATION_UNSPECIFIED:
        return {GatewayError::unsupported_operation, CommandParams{}};
    }
    return {GatewayError::unsupported_operation, CommandParams{}};
}

[[nodiscard]] pb::CommandStatus to_pb_status(ReceiptStatus status) {
    switch (status) {
    case ReceiptStatus::accepted:
        return pb::ACCEPTED;
    case ReceiptStatus::applied:
        return pb::APPLIED;
    case ReceiptStatus::rejected:
        return pb::REJECTED;
    case ReceiptStatus::duplicate:
        return pb::DUPLICATE;
    }
    return pb::COMMAND_STATUS_UNSPECIFIED;
}

[[nodiscard]] std::string read_file(const std::string& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return {};
    }
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

// gRPC 同步服务：RPC 方法只打包请求并排队，实际处理由 ControlServer::poll
// 在组合根主线程执行；处理器线程经 promise/future 等待结果。
class ControlService final : public pb::GeoWorldControl::Service {
public:
    using Job = std::function<void()>;

    ControlService(GatewayCore& core, ControlServerConfig config)
        : core_(core), config_(std::move(config)) {}

    void post(Job job) {
        std::lock_guard lock{mutex_};
        jobs_.push_back(std::move(job));
    }

    void run_jobs() {
        std::deque<Job> jobs;
        {
            std::lock_guard lock{mutex_};
            jobs.swap(jobs_);
        }
        for (Job& job : jobs) {
            job();
        }
    }

    grpc::Status OpenSession(grpc::ServerContext* /*context*/,
                             const pb::OpenSessionRequest* request,
                             pb::OpenSessionResponse* response) override {
        return dispatch([this, request, response] {
            const GatewayCore::OpenSessionResult result = core_.open_session(
                request->credential_token(), request->control_versions().minimum(),
                request->control_versions().maximum(), request->data_versions().minimum(),
                request->data_versions().maximum(), current_tick());
            if (result.error != GatewayError::none) {
                fill_error(response->mutable_error(), result.error);
                return;
            }
            response->set_session_id(encode_session_id(result.session.session.id));
            response->set_control_api_version(result.session.session.control_api_version);
            response->set_data_schema_version(result.session.session.data_schema_version);
            response->set_stream_ticket(result.session.stream_ticket);
            response->set_stream_ticket_ttl_seconds(result.session.ticket_ttl_seconds);
            response->set_data_endpoint(config_.advertised_data_endpoint);
            response->set_current_tick(result.session.session.created_tick);
        });
    }

    grpc::Status CloseSession(grpc::ServerContext* /*context*/,
                              const pb::CloseSessionRequest* request,
                              pb::CloseSessionResponse* response) override {
        return dispatch([this, request, response] {
            const std::optional<SessionId> id = decode_session_id(request->session_id());
            response->set_closed(id.has_value() && core_.close_session(*id));
        });
    }

    grpc::Status UpdateSubscription(
        grpc::ServerContext* /*context*/, const pb::UpdateSubscriptionRequest* request,
        pb::UpdateSubscriptionResponse* response) override {
        return dispatch([this, request, response] {
            const std::optional<SessionId> id = decode_session_id(request->session_id());
            if (!id.has_value()) {
                fill_error(response->mutable_error(), GatewayError::invalid_request);
                return;
            }
            GatewayError mapping_error;
            std::optional<projection::Subscription> subscription =
                to_subscription(request->subscription(), mapping_error);
            if (!subscription.has_value()) {
                fill_error(response->mutable_error(), mapping_error);
                return;
            }
            const GatewayError error = core_.update_subscription(*id, *subscription);
            response->set_updated(error == GatewayError::none);
            if (error != GatewayError::none) {
                fill_error(response->mutable_error(), error);
            }
        });
    }

    grpc::Status AcquireOwnership(
        grpc::ServerContext* /*context*/, const pb::AcquireOwnershipRequest* request,
        pb::AcquireOwnershipResponse* response) override {
        return dispatch([this, request, response] {
            const std::optional<SessionId> id = decode_session_id(request->session_id());
            if (!id.has_value()) {
                fill_error(response->mutable_error(), GatewayError::invalid_request);
                return;
            }
            const std::vector<std::string> keys{request->property_keys().begin(),
                                                request->property_keys().end()};
            const GatewayError error = core_.acquire_ownership(
                *id, foundation::WorldId{request->target_wid()}, keys,
                request->lease_until_tick(), current_tick());
            response->set_granted(error == GatewayError::none);
            response->set_lease_until_tick(request->lease_until_tick());
            if (error != GatewayError::none) {
                fill_error(response->mutable_error(), error);
            }
        });
    }

    grpc::Status ReleaseOwnership(
        grpc::ServerContext* /*context*/, const pb::ReleaseOwnershipRequest* request,
        pb::ReleaseOwnershipResponse* response) override {
        return dispatch([this, request, response] {
            const std::optional<SessionId> id = decode_session_id(request->session_id());
            if (!id.has_value()) {
                response->set_released(false);
                return;
            }
            const std::vector<std::string> keys{request->property_keys().begin(),
                                                request->property_keys().end()};
            const GatewayError error = core_.release_ownership(
                *id, foundation::WorldId{request->target_wid()}, keys);
            response->set_released(error == GatewayError::none);
        });
    }

    grpc::Status SubmitCommand(grpc::ServerContext* /*context*/,
                               const pb::SubmitCommandRequest* request,
                               pb::SubmitCommandResponse* response) override {
        return dispatch([this, request, response] {
            const std::optional<SessionId> id = decode_session_id(request->session_id());
            if (!id.has_value()) {
                response->set_status(pb::REJECTED);
                fill_error(response->mutable_error(), GatewayError::invalid_request);
                return;
            }
            const auto [mapping_error, params] = to_command_params(*request);
            if (mapping_error != GatewayError::none) {
                response->set_status(pb::REJECTED);
                fill_error(response->mutable_error(), mapping_error);
                return;
            }
            ExternalCommand command;
            command.session = *id;
            command.client_sequence = request->client_sequence();
            command.target_wid = foundation::WorldId{request->target_wid()};
            command.params = params;
            command.expected_object_version = request->expected_object_version();
            command.target_tick_hint = request->target_tick_hint();
            const auto [error, receipt] =
                core_.submit_command(*id, command, current_tick());
            response->set_status(to_pb_status(receipt.status));
            response->set_ingress_sequence(receipt.ingress_sequence);
            if (error != GatewayError::none) {
                fill_error(response->mutable_error(), error);
            }
        });
    }

    grpc::Status RequestKeyframe(grpc::ServerContext* /*context*/,
                                 const pb::RequestKeyframeRequest* request,
                                 pb::RequestKeyframeResponse* response) override {
        return dispatch([this, request, response] {
            const std::optional<SessionId> id = decode_session_id(request->session_id());
            response->set_scheduled(id.has_value()
                                    && core_.request_keyframe(*id) == GatewayError::none);
        });
    }

    grpc::Status GetServerInfo(grpc::ServerContext* /*context*/,
                               const pb::GetServerInfoRequest* /*request*/,
                               pb::GetServerInfoResponse* response) override {
        return dispatch([this, response] {
            std::ostringstream version;
            version << foundation::major_version << '.' << foundation::minor_version << '.'
                    << foundation::patch_version;
            response->set_build_version(version.str());
            response->mutable_supported_control_versions()->set_minimum(
                protocol::control_api_version);
            response->mutable_supported_control_versions()->set_maximum(
                protocol::control_api_version);
            response->mutable_supported_data_versions()->set_minimum(
                protocol::data_schema_version);
            response->mutable_supported_data_versions()->set_maximum(
                protocol::data_schema_version);
            response->set_current_tick(current_tick());
            for (const std::string& capability : config_.capabilities) {
                response->add_capabilities(capability);
            }
        });
    }

private:
    template <typename Fn>
    [[nodiscard]] grpc::Status dispatch(Fn&& handler) {
        auto done = std::make_shared<std::promise<void>>();
        std::future<void> finished = done->get_future();
        post([handler = std::forward<Fn>(handler), done]() mutable {
            handler();
            done->set_value();
        });
        // 处理器线程在此阻塞，主线程 poll() 执行 handler 后唤醒；超时保护进程关停路径。
        if (finished.wait_for(config_.dispatch_timeout) != std::future_status::ready) {
            return grpc::Status{grpc::StatusCode::UNAVAILABLE,
                                "control dispatch timeout"};
        }
        return grpc::Status::OK;
    }

    [[nodiscard]] std::uint64_t current_tick() const {
        return config_.tick_source ? config_.tick_source() : 0;
    }

    GatewayCore& core_;
    ControlServerConfig config_;
    std::mutex mutex_;
    std::deque<Job> jobs_;
};

} // namespace

struct ControlServer::Impl {
    GatewayCore& core;
    ControlServerConfig config;
    ControlService service;
    std::unique_ptr<grpc::Server> server;
    int selected_port{};

    Impl(GatewayCore& gateway_core, ControlServerConfig server_config)
        : core(gateway_core), config(std::move(server_config)),
          service(gateway_core, config) {}
};

ControlServer::ControlServer(GatewayCore& core, ControlServerConfig config)
    : impl_(std::make_unique<Impl>(core, std::move(config))) {}

ControlServer::~ControlServer() {
    shutdown();
}

[[nodiscard]] bool ControlServer::start(std::string& diagnostic) {
    if (!impl_->config.tick_source) {
        diagnostic = "ControlServerConfig.tick_source 必须提供";
        return false;
    }
    if (!is_loopback_address(impl_->config.listen_address) && !impl_->config.tls.has_value()) {
        diagnostic = "非 loopback 控制面监听必须配置 TLS 证书";
        return false;
    }

    grpc::ServerBuilder builder;
    std::shared_ptr<grpc::ServerCredentials> credentials;
    if (impl_->config.tls.has_value()) {
        const TlsCertificateFiles& tls = *impl_->config.tls;
        grpc::SslServerCredentialsOptions options;
        options.pem_key_cert_pairs.push_back(grpc::SslServerCredentialsOptions::PemKeyCertPair{
            read_file(tls.private_key_file), read_file(tls.certificate_chain_file)});
        if (options.pem_key_cert_pairs.front().private_key.empty()
            || options.pem_key_cert_pairs.front().cert_chain.empty()) {
            diagnostic = "TLS 证书或私钥文件读取失败";
            return false;
        }
        credentials = grpc::SslServerCredentials(options);
    } else {
        credentials = grpc::InsecureServerCredentials();
    }

    const std::string endpoint =
        impl_->config.listen_address + ":" + std::to_string(impl_->config.port);
    builder.AddListeningPort(endpoint, credentials, &impl_->selected_port);
    builder.RegisterService(&impl_->service);
    impl_->server = builder.BuildAndStart();
    if (!impl_->server) {
        diagnostic = "gRPC 控制面监听失败: " + endpoint;
        return false;
    }
    diagnostic.clear();
    return true;
}

void ControlServer::poll() {
    impl_->service.run_jobs();
}

void ControlServer::shutdown() {
    if (impl_->server) {
        impl_->server->Shutdown();
        impl_->server.reset();
    }
}

[[nodiscard]] std::uint16_t ControlServer::bound_port() const noexcept {
    return static_cast<std::uint16_t>(impl_->selected_port);
}

[[nodiscard]] bool is_loopback_address(std::string_view address) {
    return is_loopback_host(address);
}

} // namespace geoworld::gateway
