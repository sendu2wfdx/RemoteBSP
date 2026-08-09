#include "remotebsp/mock_mcu/remote_core.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace remotebsp::mock_mcu {
namespace {

constexpr std::uint32_t kMinimumLeaseDurationMs = 100;
constexpr std::uint32_t kMaximumLeaseDurationMs = 60000;

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        output.push_back(
            static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        output.push_back(
            static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::uint32_t read_u32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

}

CoreException::CoreException(CoreError code, const char* message)
    : std::runtime_error(message), code_(code) {}

CoreError CoreException::code() const noexcept { return code_; }

RemoteCore::RemoteCore(NodeInfo node_info, std::uint64_t capabilities,
                       std::shared_ptr<GpioBsp> gpio_bsp,
                       std::shared_ptr<UartBsp> uart_bsp,
                       std::vector<protocol::ResourceDescriptor> resources,
                       std::vector<protocol::ResourceContract> contracts,
                       std::shared_ptr<MotionExecutor> motion,
                       std::shared_ptr<WaveformBsp> waveform)
    : node_info_(node_info),
      capabilities_(capabilities),
      gpio_bsp_(std::move(gpio_bsp)),
      uart_bsp_(std::move(uart_bsp)),
      motion_(std::move(motion)),
      waveform_(std::move(waveform)),
      resources_(std::move(resources)),
      contracts_(std::move(contracts)) {
    if (node_info_.protocol_version != protocol::kProtocolVersion) {
        throw CoreException(CoreError::UnsupportedVersion,
                            "节点协议版本与远程核心不兼容");
    }
    std::unordered_set<std::uint32_t> resource_ids;
    for (const auto& resource : resources_) {
        if (resource.resource_id == 0 ||
            !resource_ids.insert(resource.resource_id).second) {
            throw CoreException(CoreError::InvalidResourceCatalog,
                                "资源目录包含零 ID 或重复 ID");
        }
    }
    std::unordered_set<std::uint32_t> contract_ids;
    for (const auto& contract : contracts_) {
        const bool lease_required =
            (contract.access_flags &
             protocol::kResourceAccessLeaseRequired) != 0U;
        const bool lease_supported =
            (contract.access_flags &
             protocol::kResourceAccessLeaseSupported) != 0U;
        if (contract.resource_id == 0 ||
            contract.version != protocol::kResourceContractVersion ||
            resource_ids.find(contract.resource_id) ==
                resource_ids.end() ||
            !contract_ids.insert(contract.resource_id).second ||
            (lease_required && !lease_supported)) {
            throw CoreException(CoreError::InvalidResourceCatalog,
                                "资源能力合同与资源目录不一致");
        }
    }
}

protocol::Packet RemoteCore::handle(const protocol::Packet& request,
                                    TimePoint now) {
    if (request.header.message_type != protocol::MessageType::Request) {
        throw CoreException(CoreError::NotRequest,
                            "远程核心只接受请求消息");
    }
    if (request.header.version != protocol::kProtocolVersion) {
        throw CoreException(CoreError::UnsupportedVersion,
                            "请求协议版本不受支持");
    }
    expire_leases(now);

    switch (static_cast<protocol::Command>(request.header.command)) {
        case protocol::Command::GetInfo:
            return handle_get_info(request);
        case protocol::Command::GetCapability:
            return handle_get_capability(request);
        case protocol::Command::Ping:
            return handle_ping(request);
        case protocol::Command::BootloaderEnter:
        case protocol::Command::BootloaderEnterUsb:
            return handle_bootloader_enter(request);
        case protocol::Command::ResourceEnum:
            return handle_resource_enum(request);
        case protocol::Command::ResourceDescribe:
            return handle_resource_describe(request);
        case protocol::Command::ResourceStatus:
            return handle_resource_status(request);
        case protocol::Command::ResourceReset:
            return handle_resource_reset(request);
        case protocol::Command::ResourceContract:
            return handle_resource_contract(request);
        case protocol::Command::ResourceAcquire:
            return handle_resource_acquire(request, now);
        case protocol::Command::ResourceRenew:
            return handle_resource_renew(request, now);
        case protocol::Command::ResourceRelease:
            return handle_resource_release(request);
        case protocol::Command::ResourceLeaseStatus:
            return handle_resource_lease_status(request, now);
        case protocol::Command::GpioCreate:
            return handle_gpio_create(request);
        case protocol::Command::GpioRead:
            return handle_gpio_read(request);
        case protocol::Command::GpioWrite:
            return handle_gpio_write(request);
        case protocol::Command::UartCreate:
            return handle_uart_create(request);
        case protocol::Command::UartRead:
            return handle_uart_read(request);
        case protocol::Command::UartWrite:
            return handle_uart_write(request);
        case protocol::Command::PwmCreate:
            return handle_pwm_create(request);
        case protocol::Command::PwmWrite:
            return handle_pwm_write(request);
        case protocol::Command::PwmStop:
            return handle_pwm_stop(request);
        case protocol::Command::TimedBitstreamCreate:
            return handle_timed_bitstream_create(request);
        case protocol::Command::TimedBitstreamWrite:
            return handle_timed_bitstream_write(request);
        case protocol::Command::TimedBitstreamAbort:
            return handle_timed_bitstream_abort(request);
        case protocol::Command::MotionEnqueue:
            return handle_motion_enqueue(request);
        case protocol::Command::MotionStatus:
            return handle_motion_status(request);
        case protocol::Command::MotionAbort:
            return handle_motion_abort(request);
        case protocol::Command::MotionClearFault:
            return handle_motion_clear_fault(request);
        default:
            return make_response(request, StatusCode::UnknownCommand);
    }
}

const NodeInfo& RemoteCore::node_info() const noexcept { return node_info_; }

std::uint64_t RemoteCore::capabilities() const noexcept {
    return capabilities_;
}

bool RemoteCore::bootloader_requested() const noexcept {
    return bootloader_requested_;
}

const protocol::ResourceDescriptor* RemoteCore::find_resource(
    std::uint32_t resource_id) const noexcept {
    const auto found = std::find_if(
        resources_.begin(), resources_.end(),
        [resource_id](const auto& resource) {
            return resource.resource_id == resource_id;
        });
    return found == resources_.end() ? nullptr : &*found;
}

const protocol::ResourceDescriptor* RemoteCore::find_resource(
    protocol::ResourceType type, std::uint16_t instance) const noexcept {
    const auto found = std::find_if(
        resources_.begin(), resources_.end(),
        [type, instance](const auto& resource) {
            return resource.type == type && resource.instance == instance;
        });
    return found == resources_.end() ? nullptr : &*found;
}

const protocol::ResourceContract* RemoteCore::find_contract(
    std::uint32_t resource_id) const noexcept {
    const auto found = std::find_if(
        contracts_.begin(), contracts_.end(),
        [resource_id](const auto& contract) {
            return contract.resource_id == resource_id;
        });
    return found == contracts_.end() ? nullptr : &*found;
}

bool RemoteCore::resource_has_objects(
    std::uint32_t resource_id) const noexcept {
    return std::any_of(
               gpio_objects_.begin(), gpio_objects_.end(),
               [resource_id](const auto& entry) {
                   return entry.second.resource_id == resource_id;
               }) ||
           std::any_of(
               uart_objects_.begin(), uart_objects_.end(),
               [resource_id](const auto& entry) {
                   return entry.second.resource_id == resource_id;
               }) ||
           std::any_of(
               pwm_objects_.begin(), pwm_objects_.end(),
               [resource_id](const auto& entry) {
                   return entry.second.resource_id == resource_id;
               }) ||
           std::any_of(
               timed_bitstream_objects_.begin(),
               timed_bitstream_objects_.end(),
               [resource_id](const auto& entry) {
                   return entry.second.resource_id == resource_id;
               });
}

bool RemoteCore::session_has_exclusive_lease(
    std::uint32_t resource_id, std::uint32_t session_id) const noexcept {
    const auto found = leases_.find(resource_id);
    if (found == leases_.end()) {
        return false;
    }
    return std::any_of(
        found->second.begin(), found->second.end(),
        [session_id](const auto& lease) {
            return lease.owner_session_id == session_id &&
                   lease.mode == protocol::ResourceLeaseMode::Exclusive;
        });
}

bool RemoteCore::resource_access_allowed(
    std::uint32_t resource_id, std::uint32_t session_id) const noexcept {
    const auto found = leases_.find(resource_id);
    if (found == leases_.end() || found->second.empty()) {
        return true;
    }
    return std::any_of(
        found->second.begin(), found->second.end(),
        [session_id](const auto& lease) {
            return lease.owner_session_id == session_id;
        });
}

void RemoteCore::release_resource_objects(
    std::uint32_t resource_id, std::uint32_t owner_session_id) {
    const auto* released_resource = find_resource(resource_id);
    if (released_resource != nullptr &&
        released_resource->type ==
            protocol::ResourceType::StepgenAxis &&
        motion_ != nullptr) {
        const auto motion_status = motion_->status();
        if (motion_status.state == MotionState::Armed ||
            motion_status.state == MotionState::Running) {
            try {
                static_cast<void>(motion_->abort(
                    motion_status.node_time_ns, MotionFault::Aborted));
            } catch (const std::exception&) {
                // 运动租约释放必须继续完成，即使执行器已被其它安全源停机。
            }
        }
    }
    for (auto iterator = gpio_objects_.begin();
         iterator != gpio_objects_.end();) {
        const auto& object = iterator->second;
        if (object.resource_id != resource_id ||
            object.owner_session_id != owner_session_id) {
            ++iterator;
            continue;
        }
        if (gpio_bsp_ && object.direction == GpioDirection::Output) {
            try {
                gpio_bsp_->write(object.pin, false);
            } catch (const std::exception&) {
                // 释放路径必须继续清理对象，底层故障由资源状态另行报告。
            }
        }
        iterator = gpio_objects_.erase(iterator);
    }

    for (auto iterator = uart_objects_.begin();
         iterator != uart_objects_.end();) {
        const auto& object = iterator->second;
        if (object.resource_id != resource_id ||
            object.owner_session_id != owner_session_id) {
            ++iterator;
            continue;
        }
        if (uart_bsp_) {
            try {
                uart_bsp_->reset(object.port);
            } catch (const std::exception&) {
                // 与 GPIO 相同，资源释放不能被后端异常阻塞。
            }
        }
        iterator = uart_objects_.erase(iterator);
    }

    for (auto iterator = pwm_objects_.begin();
         iterator != pwm_objects_.end();) {
        const auto object = iterator->second;
        if (object.resource_id != resource_id ||
            object.owner_session_id != owner_session_id) {
            ++iterator;
            continue;
        }
        if (waveform_) {
            try {
                waveform_->pwm_stop(object.channel);
                waveform_->reset_pwm(object.channel);
            } catch (const std::exception&) {
                // 释放必须继续，后端故障由资源状态单独报告。
            }
        }
        iterator = pwm_objects_.erase(iterator);
    }

    for (auto iterator = timed_bitstream_objects_.begin();
         iterator != timed_bitstream_objects_.end();) {
        const auto object = iterator->second;
        if (object.resource_id != resource_id ||
            object.owner_session_id != owner_session_id) {
            ++iterator;
            continue;
        }
        if (waveform_) {
            try {
                waveform_->bitstream_abort(object.channel);
                waveform_->reset_bitstream(object.channel);
            } catch (const std::exception&) {
                // 与 PWM 相同，始终完成对象清理。
            }
        }
        iterator = timed_bitstream_objects_.erase(iterator);
    }
}

std::size_t RemoteCore::expire_leases(TimePoint now) {
    std::size_t expired = 0;
    for (auto map_iterator = leases_.begin();
         map_iterator != leases_.end();) {
        const auto resource_id = map_iterator->first;
        auto& entries = map_iterator->second;
        for (auto iterator = entries.begin(); iterator != entries.end();) {
            if (iterator->expires_at > now) {
                ++iterator;
                continue;
            }
            release_resource_objects(resource_id,
                                     iterator->owner_session_id);
            iterator = entries.erase(iterator);
            ++expired;
        }
        if (entries.empty()) {
            map_iterator = leases_.erase(map_iterator);
        } else {
            ++map_iterator;
        }
    }
    return expired;
}

std::size_t RemoteCore::release_session(std::uint32_t session_id) {
    std::size_t released = 0;
    for (auto map_iterator = leases_.begin();
         map_iterator != leases_.end();) {
        const auto resource_id = map_iterator->first;
        auto& entries = map_iterator->second;
        for (auto iterator = entries.begin(); iterator != entries.end();) {
            if (iterator->owner_session_id != session_id) {
                ++iterator;
                continue;
            }
            release_resource_objects(resource_id, session_id);
            iterator = entries.erase(iterator);
            ++released;
        }
        if (entries.empty()) {
            map_iterator = leases_.erase(map_iterator);
        } else {
            ++map_iterator;
        }
    }
    return released;
}

protocol::ResourceLeaseInfo RemoteCore::make_lease_info(
    std::uint32_t resource_id, std::uint32_t requester_session_id,
    TimePoint now) const {
    protocol::ResourceLeaseInfo info;
    info.resource_id = resource_id;
    const auto found = leases_.find(resource_id);
    if (found == leases_.end() || found->second.empty()) {
        return info;
    }

    info.active_lease_count = static_cast<std::uint16_t>(
        std::min<std::size_t>(found->second.size(), 0xFFFFU));
    const auto owned = std::find_if(
        found->second.begin(), found->second.end(),
        [requester_session_id](const auto& lease) {
            return lease.owner_session_id == requester_session_id;
        });
    const auto selected =
        owned == found->second.end() ? found->second.begin() : owned;
    info.owner_session_id = selected->owner_session_id;
    info.granted_duration_ms = selected->granted_duration_ms;
    info.mode = selected->mode;
    if (owned != found->second.end()) {
        info.lease_id = selected->lease_id;
    }
    if (selected->expires_at > now) {
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(selected->expires_at - now).count();
        info.remaining_ms = static_cast<std::uint32_t>(
            std::min<std::int64_t>(
                remaining, std::numeric_limits<std::uint32_t>::max()));
    }
    return info;
}

protocol::Packet RemoteCore::make_response(
    const protocol::Packet& request, StatusCode status) const {
    protocol::Packet response;
    response.header.version = protocol::kProtocolVersion;
    response.header.message_type = protocol::MessageType::Response;
    response.header.command = request.header.command;
    response.header.session_id = request.header.session_id;
    response.header.request_id = request.header.request_id;
    response.header.object_id = request.header.object_id;
    response.header.flags =
        status == StatusCode::Ok ? 0U : kResponseErrorFlag;
    response.payload.push_back(static_cast<std::uint8_t>(status));
    return response;
}

protocol::Packet RemoteCore::handle_get_info(
    const protocol::Packet& request) const {
    if (!request.payload.empty()) {
        return make_response(request, StatusCode::InvalidPayload);
    }

    protocol::Packet response = make_response(request, StatusCode::Ok);
    response.payload.insert(response.payload.end(), node_info_.uuid.begin(),
                            node_info_.uuid.end());
    append_u16(response.payload, node_info_.firmware_major);
    append_u16(response.payload, node_info_.firmware_minor);
    append_u16(response.payload, node_info_.firmware_patch);
    append_u32(response.payload, node_info_.board_type);
    response.payload.push_back(node_info_.protocol_version);
    return response;
}

protocol::Packet RemoteCore::handle_get_capability(
    const protocol::Packet& request) const {
    if (!request.payload.empty()) {
        return make_response(request, StatusCode::InvalidPayload);
    }

    protocol::Packet response = make_response(request, StatusCode::Ok);
    append_u64(response.payload, capabilities_);
    return response;
}

protocol::Packet RemoteCore::handle_ping(
    const protocol::Packet& request) const {
    if (request.payload.size() >= protocol::kMaximumPayloadSize) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    response.payload.insert(response.payload.end(), request.payload.begin(),
                            request.payload.end());
    return response;
}

protocol::Packet RemoteCore::handle_bootloader_enter(
    const protocol::Packet& request) {
    static constexpr std::array<std::uint8_t, 8> confirmation{
        'R', 'B', 'S', 'P', 'B', 'O', 'O', 'T'};
    if (request.header.object_id != 0 ||
        request.payload.size() != confirmation.size() ||
        !std::equal(request.payload.begin(), request.payload.end(),
                    confirmation.begin())) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if ((capabilities_ &
         capability_mask(Capability::Bootloader)) == 0) {
        return make_response(request,
                             StatusCode::UnsupportedCapability);
    }
    bootloader_requested_ = true;
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::handle_resource_enum(
    const protocol::Packet& request) const {
    if (!request.payload.empty() || request.header.object_id != 0) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto encoded = protocol::encode_resource_list(resources_);
    if (encoded.size() + 1U > protocol::kMaximumPayloadSize) {
        return make_response(request, StatusCode::ResourceExhausted);
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_resource_describe(
    const protocol::Packet& request) const {
    if (request.header.object_id != 0 || request.payload.size() != 4) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto resource_id = protocol::decode_resource_id(request.payload);
    const auto found = std::find_if(
        resources_.begin(), resources_.end(),
        [resource_id](const auto& resource) {
            return resource.resource_id == resource_id;
        });
    if (found == resources_.end()) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_resource_descriptor(*found);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_resource_status(
    const protocol::Packet& request) const {
    if (request.header.object_id != 0 || request.payload.size() != 4) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto resource_id = protocol::decode_resource_id(request.payload);
    const auto found = std::find_if(
        resources_.begin(), resources_.end(),
        [resource_id](const auto& resource) {
            return resource.resource_id == resource_id;
        });
    if (found == resources_.end()) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    protocol::ResourceStatusPayload status{
        resource_id, protocol::ResourceHealth::Normal, 0, 0, 0, 0, 0};
    if (found->type == protocol::ResourceType::Uart && uart_bsp_ &&
        found->instance <= std::numeric_limits<std::uint8_t>::max()) {
        try {
            const auto uart_status = uart_bsp_->status(
                static_cast<std::uint8_t>(found->instance));
            status.rx_buffered = uart_status.rx_buffered;
            status.tx_buffered = uart_status.tx_buffered;
            status.rx_overruns = uart_status.rx_overruns;
            status.tx_overruns = uart_status.tx_overruns;
            if (uart_status.rx_overruns != 0) {
                status.error_flags |= protocol::kResourceErrorRxOverflow;
            }
            if (uart_status.tx_overruns != 0) {
                status.error_flags |= protocol::kResourceErrorTxOverflow;
            }
            if (found->rx_capacity != 0 &&
                static_cast<std::uint64_t>(uart_status.rx_buffered) * 4U >=
                    static_cast<std::uint64_t>(found->rx_capacity) * 3U) {
                status.error_flags |=
                    protocol::kResourceErrorRxHighWater;
            }
            if (found->tx_capacity != 0 &&
                static_cast<std::uint64_t>(uart_status.tx_buffered) * 4U >=
                    static_cast<std::uint64_t>(found->tx_capacity) * 3U) {
                status.error_flags |=
                    protocol::kResourceErrorTxHighWater;
            }
            if (uart_status.failed) {
                status.error_flags |=
                    protocol::kResourceErrorBackendFailure;
                status.health = protocol::ResourceHealth::Failed;
            } else if ((status.error_flags &
                        (protocol::kResourceErrorRxOverflow |
                         protocol::kResourceErrorTxOverflow)) != 0) {
                status.health = protocol::ResourceHealth::Degraded;
            } else if (status.error_flags != 0) {
                status.health = protocol::ResourceHealth::Busy;
            }
        } catch (const std::exception&) {
            status.error_flags |= protocol::kResourceErrorBackendFailure;
            status.health = protocol::ResourceHealth::Failed;
        }
    } else if (found->type ==
                   protocol::ResourceType::StepgenAxis &&
               motion_) {
        const auto motion_status = motion_->status();
        if (motion_status.fault != MotionFault::None) {
            status.error_flags |=
                protocol::kResourceErrorBackendFailure;
            status.health = protocol::ResourceHealth::Failed;
        } else if (motion_status.state == MotionState::Armed ||
                   motion_status.state == MotionState::Running) {
            status.health = protocol::ResourceHealth::Busy;
        }
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_resource_status(status);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_resource_reset(
    const protocol::Packet& request) {
    if (request.header.object_id != 0 || request.payload.size() != 4) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto resource_id = protocol::decode_resource_id(request.payload);
    const auto found = std::find_if(
        resources_.begin(), resources_.end(),
        [resource_id](const auto& resource) {
            return resource.resource_id == resource_id;
        });
    if (found == resources_.end()) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    if (!resource_access_allowed(resource_id,
                                 request.header.session_id) ||
        (leases_.find(resource_id) != leases_.end() &&
         !session_has_exclusive_lease(resource_id,
                                      request.header.session_id))) {
        return make_response(request, StatusCode::AccessDenied);
    }
    if (found->type == protocol::ResourceType::StepgenAxis && motion_) {
        try {
            const auto motion_status = motion_->status();
            if (motion_status.state == MotionState::Armed ||
                motion_status.state == MotionState::Running) {
                static_cast<void>(motion_->abort(
                    motion_status.node_time_ns, MotionFault::Aborted));
            }
            motion_->clear_fault();
        } catch (const std::exception&) {
            return make_response(request, StatusCode::ResourceFailed);
        }
        return make_response(request, StatusCode::Ok);
    }
    if (found->type == protocol::ResourceType::Pwm && waveform_ &&
        found->instance <= std::numeric_limits<std::uint8_t>::max()) {
        try {
            waveform_->pwm_stop(static_cast<std::uint8_t>(found->instance));
        } catch (const std::exception&) {
            // 尚未创建对象时复位仍然是幂等成功。
        }
        release_resource_objects(resource_id, request.header.session_id);
        return make_response(request, StatusCode::Ok);
    }
    if (found->type == protocol::ResourceType::TimedBitstream && waveform_ &&
        found->instance <= std::numeric_limits<std::uint8_t>::max()) {
        try {
            waveform_->bitstream_abort(
                static_cast<std::uint8_t>(found->instance));
        } catch (const std::exception&) {
            // 与 PWM 一样允许对空闲资源重复复位。
        }
        release_resource_objects(resource_id, request.header.session_id);
        return make_response(request, StatusCode::Ok);
    }
    if (found->type != protocol::ResourceType::Uart || !uart_bsp_ ||
        found->instance > std::numeric_limits<std::uint8_t>::max()) {
        return make_response(request,
                             StatusCode::UnsupportedCapability);
    }
    try {
        uart_bsp_->reset(static_cast<std::uint8_t>(found->instance));
    } catch (const std::exception& error) {
        return make_uart_error_response(request, error);
    }
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::handle_resource_contract(
    const protocol::Packet& request) const {
    if (request.header.object_id != 0 || request.payload.size() != 4) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto resource_id = protocol::decode_resource_id(request.payload);
    if (find_resource(resource_id) == nullptr) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    const auto* contract = find_contract(resource_id);
    if (contract == nullptr) {
        return make_response(request,
                             StatusCode::UnsupportedCapability);
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_resource_contract(*contract);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_resource_acquire(
    const protocol::Packet& request, TimePoint now) {
    if (request.header.object_id != 0 ||
        request.header.session_id == 0) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::ResourceLeaseRequest lease_request;
    try {
        lease_request =
            protocol::decode_resource_lease_request(request.payload);
    } catch (const protocol::ResourcePayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (lease_request.duration_ms < kMinimumLeaseDurationMs ||
        lease_request.duration_ms > kMaximumLeaseDurationMs) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (find_resource(lease_request.resource_id) == nullptr) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    const auto* contract = find_contract(lease_request.resource_id);
    if (contract == nullptr ||
        (contract->access_flags &
         protocol::kResourceAccessLeaseSupported) == 0U) {
        return make_response(request,
                             StatusCode::UnsupportedCapability);
    }
    if (lease_request.mode == protocol::ResourceLeaseMode::SharedRead &&
        (contract->access_flags &
         protocol::kResourceAccessSharedRead) == 0U) {
        return make_response(request, StatusCode::AccessDenied);
    }

    auto& entries = leases_[lease_request.resource_id];
    const bool session_already_owns = std::any_of(
        entries.begin(), entries.end(),
        [&](const auto& lease) {
            return lease.owner_session_id ==
                   request.header.session_id;
        });
    const bool has_exclusive = std::any_of(
        entries.begin(), entries.end(),
        [](const auto& lease) {
            return lease.mode ==
                   protocol::ResourceLeaseMode::Exclusive;
        });
    if (session_already_owns ||
        (lease_request.mode == protocol::ResourceLeaseMode::Exclusive &&
         (!entries.empty() ||
          resource_has_objects(lease_request.resource_id))) ||
        (lease_request.mode == protocol::ResourceLeaseMode::SharedRead &&
         (has_exclusive ||
          resource_has_objects(lease_request.resource_id)))) {
        if (entries.empty()) {
            leases_.erase(lease_request.resource_id);
        }
        return make_response(request, StatusCode::ResourceBusy);
    }

    const std::uint64_t lease_id = next_lease_id_++;
    if (next_lease_id_ == 0) {
        next_lease_id_ = 1;
    }
    entries.push_back(
        {lease_id,
         request.header.session_id,
         lease_request.duration_ms,
         lease_request.mode,
         now + std::chrono::milliseconds(lease_request.duration_ms)});
    protocol::Packet response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_resource_lease_info(
        make_lease_info(lease_request.resource_id,
                        request.header.session_id, now));
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_resource_renew(
    const protocol::Packet& request, TimePoint now) {
    if (request.header.object_id != 0 ||
        request.header.session_id == 0) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::ResourceLeaseTokenRequest renew;
    try {
        renew =
            protocol::decode_resource_lease_token_request(request.payload);
    } catch (const protocol::ResourcePayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (renew.lease_id == 0 ||
        renew.duration_ms < kMinimumLeaseDurationMs ||
        renew.duration_ms > kMaximumLeaseDurationMs) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (find_resource(renew.resource_id) == nullptr) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    const auto found = leases_.find(renew.resource_id);
    if (found == leases_.end()) {
        return make_response(request, StatusCode::AccessDenied);
    }
    const auto lease = std::find_if(
        found->second.begin(), found->second.end(),
        [&](const auto& entry) {
            return entry.lease_id == renew.lease_id &&
                   entry.owner_session_id ==
                       request.header.session_id;
        });
    if (lease == found->second.end()) {
        return make_response(request, StatusCode::AccessDenied);
    }
    lease->granted_duration_ms = renew.duration_ms;
    lease->expires_at =
        now + std::chrono::milliseconds(renew.duration_ms);
    protocol::Packet response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_resource_lease_info(
        make_lease_info(renew.resource_id,
                        request.header.session_id, now));
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_resource_release(
    const protocol::Packet& request) {
    if (request.header.object_id != 0 ||
        request.header.session_id == 0) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::ResourceLeaseTokenRequest release;
    try {
        release =
            protocol::decode_resource_lease_token_request(request.payload);
    } catch (const protocol::ResourcePayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (release.lease_id == 0 || release.duration_ms != 0) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (find_resource(release.resource_id) == nullptr) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    const auto found = leases_.find(release.resource_id);
    if (found == leases_.end()) {
        return make_response(request, StatusCode::AccessDenied);
    }
    auto& entries = found->second;
    const auto lease = std::find_if(
        entries.begin(), entries.end(),
        [&](const auto& entry) {
            return entry.lease_id == release.lease_id &&
                   entry.owner_session_id ==
                       request.header.session_id;
        });
    if (lease == entries.end()) {
        return make_response(request, StatusCode::AccessDenied);
    }
    release_resource_objects(release.resource_id,
                             request.header.session_id);
    entries.erase(lease);
    if (entries.empty()) {
        leases_.erase(found);
    }
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::handle_resource_lease_status(
    const protocol::Packet& request, TimePoint now) const {
    if (request.header.object_id != 0 || request.payload.size() != 4) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto resource_id = protocol::decode_resource_id(request.payload);
    if (find_resource(resource_id) == nullptr) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    const auto* contract = find_contract(resource_id);
    if (contract == nullptr ||
        (contract->access_flags &
         protocol::kResourceAccessLeaseSupported) == 0U) {
        return make_response(request,
                             StatusCode::UnsupportedCapability);
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    auto lease_info =
        make_lease_info(resource_id, request.header.session_id, now);
    // 状态查询不返回能力令牌，租约 ID 只在取得和续租成功时返回。
    lease_info.lease_id = 0;
    const auto encoded =
        protocol::encode_resource_lease_info(lease_info);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_gpio_create(
    const protocol::Packet& request) {
    if (!gpio_bsp_ ||
        (capabilities_ & capability_mask(Capability::Gpio)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0 || request.payload.size() != 4 ||
        request.payload[2] >
            static_cast<std::uint8_t>(GpioDirection::Output) ||
        request.payload[3] > 1U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (next_object_id_ == 0 ||
        gpio_objects_.size() >=
            std::numeric_limits<std::uint32_t>::max() - 1ULL) {
        return make_response(request, StatusCode::ResourceExhausted);
    }

    const std::uint16_t pin = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(request.payload[0]) |
        (static_cast<std::uint16_t>(request.payload[1]) << 8U));
    const auto direction =
        static_cast<GpioDirection>(request.payload[2]);
    const bool initial_value = request.payload[3] != 0;
    const bool catalog_has_gpio = std::any_of(
        resources_.begin(), resources_.end(), [](const auto& resource) {
            return resource.type == protocol::ResourceType::Gpio;
        });
    const auto* resource =
        find_resource(protocol::ResourceType::Gpio, pin);
    if (catalog_has_gpio && resource == nullptr) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    std::uint32_t owner_session_id = 0;
    if (resource != nullptr) {
        const auto* contract = find_contract(resource->resource_id);
        const bool lease_required =
            contract != nullptr &&
            (contract->access_flags &
             protocol::kResourceAccessLeaseRequired) != 0U;
        const bool lease_active =
            leases_.find(resource->resource_id) != leases_.end();
        if ((lease_required || lease_active) &&
            !session_has_exclusive_lease(
                resource->resource_id, request.header.session_id)) {
            return make_response(request, StatusCode::AccessDenied);
        }
        if (session_has_exclusive_lease(
                resource->resource_id, request.header.session_id)) {
            owner_session_id = request.header.session_id;
        }
    }
    const auto gpio_in_use = std::find_if(
        gpio_objects_.begin(), gpio_objects_.end(),
        [pin](const auto& entry) { return entry.second.pin == pin; });
    if (gpio_in_use != gpio_objects_.end()) {
        return make_response(request, StatusCode::ResourceBusy);
    }
    gpio_bsp_->configure(pin, direction, initial_value);

    const std::uint32_t object_id = next_object_id_++;
    gpio_objects_.emplace(
        object_id,
        GpioObject{pin, direction,
                   resource == nullptr ? 0U : resource->resource_id,
                   owner_session_id});
    protocol::Packet response = make_response(request, StatusCode::Ok);
    response.header.object_id = object_id;
    return response;
}

protocol::Packet RemoteCore::handle_gpio_read(
    const protocol::Packet& request) const {
    if (!gpio_bsp_ ||
        (capabilities_ & capability_mask(Capability::Gpio)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (!request.payload.empty() || request.header.object_id == 0) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto found = gpio_objects_.find(request.header.object_id);
    if (found == gpio_objects_.end()) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    if (found->second.owner_session_id != 0 &&
        found->second.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    response.payload.push_back(gpio_bsp_->read(found->second.pin) ? 1U : 0U);
    return response;
}

protocol::Packet RemoteCore::handle_gpio_write(
    const protocol::Packet& request) {
    if (!gpio_bsp_ ||
        (capabilities_ & capability_mask(Capability::Gpio)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.payload.size() != 1 || request.payload[0] > 1U ||
        request.header.object_id == 0) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto found = gpio_objects_.find(request.header.object_id);
    if (found == gpio_objects_.end()) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    if (found->second.owner_session_id != 0 &&
        found->second.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    if (found->second.direction != GpioDirection::Output) {
        return make_response(request, StatusCode::AccessDenied);
    }
    gpio_bsp_->write(found->second.pin, request.payload[0] != 0);
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::handle_uart_create(
    const protocol::Packet& request) {
    if (!uart_bsp_ ||
        (capabilities_ & capability_mask(Capability::Uart)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0 ||
        (request.payload.size() != 8 && request.payload.size() != 9)) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const std::uint32_t baud_rate = read_u32(request.payload.data() + 1);
    const std::uint8_t data_bits = request.payload[5];
    const std::uint8_t stop_bits = request.payload[6];
    const std::uint8_t parity = request.payload[7];
    if (baud_rate == 0 || data_bits < 5 || data_bits > 8 ||
        (stop_bits != 1 && stop_bits != 2) ||
        parity > static_cast<std::uint8_t>(UartParity::Even) ||
        (request.payload.size() == 9 && request.payload[8] > 1U)) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (next_object_id_ == 0) {
        return make_response(request, StatusCode::ResourceExhausted);
    }

    const std::uint8_t port = request.payload[0];
    const bool catalog_has_uart = std::any_of(
        resources_.begin(), resources_.end(), [](const auto& resource) {
            return resource.type == protocol::ResourceType::Uart;
        });
    const auto* resource =
        find_resource(protocol::ResourceType::Uart, port);
    if (catalog_has_uart && resource == nullptr) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    std::uint32_t owner_session_id = 0;
    if (resource != nullptr) {
        const auto* contract = find_contract(resource->resource_id);
        const bool lease_required =
            contract != nullptr &&
            (contract->access_flags &
             protocol::kResourceAccessLeaseRequired) != 0U;
        const bool lease_active =
            leases_.find(resource->resource_id) != leases_.end();
        if ((lease_required || lease_active) &&
            !session_has_exclusive_lease(
                resource->resource_id, request.header.session_id)) {
            return make_response(request, StatusCode::AccessDenied);
        }
        if (session_has_exclusive_lease(
                resource->resource_id, request.header.session_id)) {
            owner_session_id = request.header.session_id;
        }
    }
    const auto uart_in_use = std::find_if(
        uart_objects_.begin(), uart_objects_.end(),
        [port](const auto& entry) { return entry.second.port == port; });
    if (uart_in_use != uart_objects_.end()) {
        return make_response(request, StatusCode::ResourceBusy);
    }
    try {
        uart_bsp_->configure(
            port, {baud_rate, data_bits, stop_bits,
                   static_cast<UartParity>(parity)});
    } catch (const std::exception& error) {
        return make_uart_error_response(request, error);
    }
    const std::uint32_t object_id = next_object_id_++;
    uart_objects_.emplace(
        object_id,
        UartObject{port,
                   resource == nullptr ? 0U : resource->resource_id,
                   owner_session_id,
                   request.payload.size() == 9 &&
                       request.payload[8] == 1U,
                   0U});
    protocol::Packet response = make_response(request, StatusCode::Ok);
    response.header.object_id = object_id;
    return response;
}

protocol::Packet RemoteCore::handle_uart_read(
    const protocol::Packet& request) {
    if (!uart_bsp_ ||
        (capabilities_ & capability_mask(Capability::Uart)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id == 0 || request.payload.size() != 2) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const std::size_t maximum_length = static_cast<std::size_t>(
        static_cast<std::uint16_t>(request.payload[0]) |
        (static_cast<std::uint16_t>(request.payload[1]) << 8U));
    if (maximum_length == 0 ||
        maximum_length > protocol::kMaximumPayloadSize - 1U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto found = uart_objects_.find(request.header.object_id);
    if (found == uart_objects_.end()) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    if (found->second.owner_session_id != 0 &&
        found->second.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    if (found->second.streaming) {
        return make_response(request, StatusCode::AccessDenied);
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    std::vector<std::uint8_t> data;
    try {
        data = uart_bsp_->read(found->second.port, maximum_length);
    } catch (const std::exception& error) {
        return make_uart_error_response(request, error);
    }
    if (data.size() > maximum_length ||
        data.size() > protocol::kMaximumPayloadSize - 1U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    response.payload.insert(response.payload.end(), data.begin(), data.end());
    return response;
}

std::vector<protocol::Packet> RemoteCore::poll_uart_events(
    std::size_t maximum_payload) {
    if (maximum_payload == 0 ||
        maximum_payload > protocol::kMaximumPayloadSize) {
        throw std::invalid_argument("UART 事件载荷长度无效");
    }
    std::vector<protocol::Packet> events;
    if (!uart_bsp_) {
        return events;
    }
    for (auto& [object_id, object] : uart_objects_) {
        if (!object.streaming) {
            continue;
        }
        std::vector<std::uint8_t> data;
        try {
            data = uart_bsp_->read(object.port, maximum_payload);
        } catch (const UartException&) {
            continue;
        }
        if (data.empty()) {
            continue;
        }
        protocol::Packet event;
        event.header.message_type = protocol::MessageType::Event;
        event.header.command = static_cast<std::uint16_t>(
            protocol::Command::UartRxEvent);
        event.header.request_id = ++object.event_sequence;
        event.header.object_id = object_id;
        event.payload = std::move(data);
        events.push_back(std::move(event));
    }
    return events;
}

protocol::Packet RemoteCore::handle_uart_write(
    const protocol::Packet& request) {
    if (!uart_bsp_ ||
        (capabilities_ & capability_mask(Capability::Uart)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id == 0 || request.payload.empty()) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto found = uart_objects_.find(request.header.object_id);
    if (found == uart_objects_.end()) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    if (found->second.owner_session_id != 0 &&
        found->second.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    try {
        uart_bsp_->write(found->second.port, request.payload);
    } catch (const std::exception& error) {
        return make_uart_error_response(request, error);
    }
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::handle_pwm_create(
    const protocol::Packet& request) {
    if (!waveform_ ||
        (capabilities_ & capability_mask(Capability::Pwm)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0 || next_object_id_ == 0) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::PwmCreatePayload config;
    try {
        config = protocol::decode_pwm_create(request.payload);
    } catch (const protocol::WaveformPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const bool catalog_has_type = std::any_of(
        resources_.begin(), resources_.end(), [](const auto& resource) {
            return resource.type == protocol::ResourceType::Pwm;
        });
    const auto* resource = find_resource(
        protocol::ResourceType::Pwm, config.channel);
    if (catalog_has_type && resource == nullptr) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    if (std::any_of(pwm_objects_.begin(), pwm_objects_.end(),
                    [&](const auto& entry) {
                        return entry.second.channel == config.channel;
                    })) {
        return make_response(request, StatusCode::ResourceBusy);
    }
    std::uint32_t owner_session_id = 0;
    if (resource != nullptr) {
        const auto* contract = find_contract(resource->resource_id);
        const bool lease_required = contract != nullptr &&
            (contract->access_flags &
             protocol::kResourceAccessLeaseRequired) != 0U;
        const bool lease_active =
            leases_.find(resource->resource_id) != leases_.end();
        if ((lease_required || lease_active) &&
            !session_has_exclusive_lease(
                resource->resource_id, request.header.session_id)) {
            return make_response(request, StatusCode::AccessDenied);
        }
        if (session_has_exclusive_lease(
                resource->resource_id, request.header.session_id)) {
            owner_session_id = request.header.session_id;
        }
    }
    try {
        waveform_->pwm_configure(config);
    } catch (const std::exception&) {
        return make_response(request, StatusCode::ResourceFailed);
    }
    const std::uint32_t object_id = next_object_id_++;
    pwm_objects_.emplace(
        object_id,
        PwmObject{config.channel,
                  resource == nullptr ? 0U : resource->resource_id,
                  owner_session_id});
    auto response = make_response(request, StatusCode::Ok);
    response.header.object_id = object_id;
    return response;
}

protocol::Packet RemoteCore::handle_pwm_write(
    const protocol::Packet& request) {
    if (!waveform_ ||
        (capabilities_ & capability_mask(Capability::Pwm)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    const auto found = pwm_objects_.find(request.header.object_id);
    if (request.header.object_id == 0 || found == pwm_objects_.end()) {
        return make_response(
            request, request.header.object_id == 0
                         ? StatusCode::InvalidPayload
                         : StatusCode::ObjectNotFound);
    }
    if (found->second.owner_session_id != 0 &&
        found->second.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    try {
        waveform_->pwm_write(
            found->second.channel,
            protocol::decode_pwm_duty(request.payload));
    } catch (const protocol::WaveformPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    } catch (const std::exception&) {
        return make_response(request, StatusCode::ResourceFailed);
    }
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::handle_pwm_stop(
    const protocol::Packet& request) {
    if (!waveform_ ||
        (capabilities_ & capability_mask(Capability::Pwm)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    const auto found = pwm_objects_.find(request.header.object_id);
    if (request.header.object_id == 0 || !request.payload.empty()) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (found == pwm_objects_.end()) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    if (found->second.owner_session_id != 0 &&
        found->second.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    try {
        waveform_->pwm_stop(found->second.channel);
    } catch (const std::exception&) {
        return make_response(request, StatusCode::ResourceFailed);
    }
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::handle_timed_bitstream_create(
    const protocol::Packet& request) {
    if (!waveform_ ||
        (capabilities_ &
         capability_mask(Capability::TimedBitstream)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0 || next_object_id_ == 0) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::TimedBitstreamCreatePayload config;
    try {
        config = protocol::decode_timed_bitstream_create(request.payload);
    } catch (const protocol::WaveformPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const bool catalog_has_type = std::any_of(
        resources_.begin(), resources_.end(), [](const auto& resource) {
            return resource.type ==
                   protocol::ResourceType::TimedBitstream;
        });
    const auto* resource = find_resource(
        protocol::ResourceType::TimedBitstream, config.channel);
    if (catalog_has_type && resource == nullptr) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    if (std::any_of(timed_bitstream_objects_.begin(),
                    timed_bitstream_objects_.end(),
                    [&](const auto& entry) {
                        return entry.second.channel == config.channel;
                    })) {
        return make_response(request, StatusCode::ResourceBusy);
    }
    std::uint32_t owner_session_id = 0;
    if (resource != nullptr) {
        const auto* contract = find_contract(resource->resource_id);
        const bool lease_required = contract != nullptr &&
            (contract->access_flags &
             protocol::kResourceAccessLeaseRequired) != 0U;
        const bool lease_active =
            leases_.find(resource->resource_id) != leases_.end();
        if ((lease_required || lease_active) &&
            !session_has_exclusive_lease(
                resource->resource_id, request.header.session_id)) {
            return make_response(request, StatusCode::AccessDenied);
        }
        if (session_has_exclusive_lease(
                resource->resource_id, request.header.session_id)) {
            owner_session_id = request.header.session_id;
        }
    }
    try {
        waveform_->bitstream_configure(config);
    } catch (const std::exception&) {
        return make_response(request, StatusCode::ResourceFailed);
    }
    const std::uint32_t object_id = next_object_id_++;
    timed_bitstream_objects_.emplace(
        object_id,
        TimedBitstreamObject{
            config.channel,
            resource == nullptr ? 0U : resource->resource_id,
            owner_session_id});
    auto response = make_response(request, StatusCode::Ok);
    response.header.object_id = object_id;
    return response;
}

protocol::Packet RemoteCore::handle_timed_bitstream_write(
    const protocol::Packet& request) {
    if (!waveform_ ||
        (capabilities_ &
         capability_mask(Capability::TimedBitstream)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    const auto found =
        timed_bitstream_objects_.find(request.header.object_id);
    if (request.header.object_id == 0 ||
        found == timed_bitstream_objects_.end()) {
        return make_response(
            request, request.header.object_id == 0
                         ? StatusCode::InvalidPayload
                         : StatusCode::ObjectNotFound);
    }
    if (found->second.owner_session_id != 0 &&
        found->second.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    try {
        const auto decoded =
            protocol::decode_timed_bitstream_write(request.payload);
        waveform_->bitstream_write(
            found->second.channel, decoded.bit_count, decoded.data);
    } catch (const protocol::WaveformPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    } catch (const std::exception&) {
        return make_response(request, StatusCode::ResourceFailed);
    }
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::handle_timed_bitstream_abort(
    const protocol::Packet& request) {
    if (!waveform_ ||
        (capabilities_ &
         capability_mask(Capability::TimedBitstream)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    const auto found =
        timed_bitstream_objects_.find(request.header.object_id);
    if (request.header.object_id == 0 || !request.payload.empty()) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (found == timed_bitstream_objects_.end()) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    if (found->second.owner_session_id != 0 &&
        found->second.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    try {
        waveform_->bitstream_abort(found->second.channel);
    } catch (const std::exception&) {
        return make_response(request, StatusCode::ResourceFailed);
    }
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::handle_motion_enqueue(
    const protocol::Packet& request) {
    if (!motion_ ||
        (capabilities_ & capability_mask(Capability::Motion)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::MotionSegmentPayload decoded;
    try {
        decoded = protocol::decode_motion_segment(request.payload);
    } catch (const protocol::MotionPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    for (const auto& axis : decoded.axes) {
        const auto* resource = find_resource(axis.resource_id);
        if (resource == nullptr ||
            resource->type != protocol::ResourceType::StepgenAxis) {
            return make_response(request, StatusCode::ObjectNotFound);
        }
        const auto* contract = find_contract(axis.resource_id);
        const bool lease_required =
            contract != nullptr &&
            (contract->access_flags &
             protocol::kResourceAccessLeaseRequired) != 0U;
        const bool lease_active =
            leases_.find(axis.resource_id) != leases_.end();
        if ((lease_required || lease_active) &&
            !session_has_exclusive_lease(
                axis.resource_id, request.header.session_id)) {
            return make_response(request, StatusCode::AccessDenied);
        }
    }
    MotionSegment segment;
    segment.sequence = decoded.sequence;
    segment.start_time_ns = decoded.start_time_ns;
    segment.duration_ns = decoded.duration_ns;
    segment.final_segment = decoded.final_segment;
    segment.axes.reserve(decoded.axes.size());
    for (const auto& axis : decoded.axes) {
        segment.axes.push_back({axis.resource_id, axis.steps});
    }
    try {
        const auto accepted = motion_->enqueue(
            std::move(segment), motion_->status().node_time_ns);
        decoded.start_time_ns = accepted.start_time_ns;
    } catch (const MotionException& error) {
        if (error.code() == MotionError::QueueFull) {
            return make_response(
                request, StatusCode::ResourceExhausted);
        }
        if (error.code() == MotionError::FaultLatched) {
            return make_response(request, StatusCode::ResourceFailed);
        }
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_motion_acceptance(
        {decoded.sequence, decoded.start_time_ns,
         decoded.duration_ns, decoded.final_segment});
    response.payload.insert(
        response.payload.end(), encoded.begin(), encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_motion_status(
    const protocol::Packet& request) const {
    if (!motion_ ||
        (capabilities_ & capability_mask(Capability::Motion)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0 || !request.payload.empty()) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto source = motion_->status();
    protocol::MotionStatusPayload status;
    status.state =
        static_cast<protocol::MotionStatePayload>(source.state);
    status.fault =
        static_cast<protocol::MotionFaultPayload>(source.fault);
    status.node_time_ns = source.node_time_ns;
    status.queue_depth =
        static_cast<std::uint16_t>(source.queue_depth);
    status.queue_capacity =
        static_cast<std::uint16_t>(source.queue_capacity);
    status.last_accepted_sequence = source.last_accepted_sequence;
    status.last_completed_sequence = source.last_completed_sequence;
    status.metrics = {
        source.metrics.accepted_segments,
        source.metrics.rejected_segments,
        source.metrics.completed_segments,
        source.metrics.emitted_edges,
        source.metrics.emitted_steps,
        source.metrics.safety_stops,
        source.metrics.limit_stops,
        source.metrics.queue_underruns,
        static_cast<std::uint16_t>(
            source.metrics.maximum_queue_depth)};
    status.axes.reserve(source.axes.size());
    for (const auto& axis : source.axes) {
        status.axes.push_back(
            {axis.resource_id, axis.enabled,
             axis.direction_positive, axis.step_level,
             axis.position_steps, axis.emitted_steps});
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_motion_status(status);
    response.payload.insert(
        response.payload.end(), encoded.begin(), encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_motion_abort(
    const protocol::Packet& request) {
    if (!motion_ ||
        (capabilities_ & capability_mask(Capability::Motion)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0 || !request.payload.empty()) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    try {
        static_cast<void>(motion_->abort(
            motion_->status().node_time_ns, MotionFault::Aborted));
    } catch (const MotionException& error) {
        return make_response(
            request,
            error.code() == MotionError::FaultLatched
                ? StatusCode::ResourceFailed
                : StatusCode::InvalidPayload);
    }
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::handle_motion_clear_fault(
    const protocol::Packet& request) {
    if (!motion_ ||
        (capabilities_ & capability_mask(Capability::Motion)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0 || !request.payload.empty()) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    try {
        motion_->clear_fault();
    } catch (const MotionException&) {
        return make_response(request, StatusCode::ResourceBusy);
    }
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::make_uart_error_response(
    const protocol::Packet& request, const std::exception& error) const {
    const auto* uart_error = dynamic_cast<const UartException*>(&error);
    if (uart_error == nullptr) {
        return make_response(request, StatusCode::ResourceFailed);
    }
    switch (uart_error->code()) {
        case UartError::PortNotConfigured:
            return make_response(request, StatusCode::ObjectNotFound);
        case UartError::BufferOverflow:
            return make_response(request, StatusCode::ResourceExhausted);
        case UartError::PortFailed:
            return make_response(request, StatusCode::ResourceFailed);
    }
    return make_response(request, StatusCode::ResourceFailed);
}

}
