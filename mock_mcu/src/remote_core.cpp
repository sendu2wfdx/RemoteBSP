#include "remotebsp/mock_mcu/remote_core.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace remotebsp::mock_mcu {
namespace {

constexpr std::uint32_t kMinimumLeaseDurationMs = 100;
constexpr std::uint32_t kMaximumLeaseDurationMs = 60000;
constexpr std::size_t kMaximumStreamSessions = 16;

bool stream_capacity(const StreamBsp& bsp,
                     const protocol::StreamContract& contract,
                     std::uint32_t resource_id,
                     std::uint32_t& buffered,
                     std::uint32_t& available) noexcept {
    const auto count = bsp.buffered_bytes(resource_id);
    if (count > contract.buffer_capacity_bytes) {
        buffered = 0U;
        available = 0U;
        return false;
    }
    buffered = static_cast<std::uint32_t>(count);
    available = contract.buffer_capacity_bytes - buffered;
    return true;
}

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
                       std::shared_ptr<WaveformBsp> waveform,
                       std::shared_ptr<DeviceParameterStore>
                           device_parameters,
                       std::shared_ptr<BusBsp> bus_bsp,
                       std::shared_ptr<TimeSyncBsp> time_sync_bsp,
                       std::shared_ptr<StreamBsp> stream_bsp)
    : node_info_(node_info),
      capabilities_(capabilities),
      gpio_bsp_(std::move(gpio_bsp)),
      uart_bsp_(std::move(uart_bsp)),
      motion_(std::move(motion)),
      waveform_(std::move(waveform)),
      device_parameters_(std::move(device_parameters)),
      bus_bsp_(std::move(bus_bsp)),
      time_sync_bsp_(time_sync_bsp != nullptr
                         ? std::move(time_sync_bsp)
                         : std::make_shared<MockTimeSyncBsp>()),
      stream_bsp_(std::move(stream_bsp)),
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
    const auto mock_clock =
        std::dynamic_pointer_cast<MockTimeSyncBsp>(time_sync_bsp_);
    if (motion_ != nullptr && mock_clock != nullptr) {
        motion_group_ = std::make_shared<MockMotionGroupParticipant>(
            motion_, mock_clock);
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
        case protocol::Command::TimeSync:
            return handle_time_sync(request, now);
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
        case protocol::Command::DeviceParameterStatus:
            return handle_device_parameter_status(request, now);
        case protocol::Command::DeviceParameterList:
            return handle_device_parameter_list(request);
        case protocol::Command::DeviceParameterRead:
            return handle_device_parameter_read(request);
        case protocol::Command::DeviceParameterUnlock:
            return handle_device_parameter_unlock(request, now);
        case protocol::Command::DeviceParameterWrite:
            return handle_device_parameter_write(request, now);
        case protocol::Command::DeviceParameterLock:
            return handle_device_parameter_lock(request);
        case protocol::Command::GpioCreate:
            return handle_gpio_create(request);
        case protocol::Command::GpioRead:
            return handle_gpio_read(request);
        case protocol::Command::GpioWrite:
            return handle_gpio_write(request);
        case protocol::Command::GpioClose:
            return handle_gpio_close(request);
        case protocol::Command::UartCreate:
            return handle_uart_create(request);
        case protocol::Command::UartRead:
            return handle_uart_read(request);
        case protocol::Command::UartWrite:
            return handle_uart_write(request);
        case protocol::Command::I2cContract:
            return handle_i2c_contract(request);
        case protocol::Command::I2cTransfer:
            return handle_i2c_transfer(request);
        case protocol::Command::SpiContract:
            return handle_spi_contract(request);
        case protocol::Command::SpiTransfer:
            return handle_spi_transfer(request);
        case protocol::Command::StreamContract:
            return handle_stream_contract(request);
        case protocol::Command::StreamOpen:
            return handle_stream_open(request);
        case protocol::Command::StreamData:
            return handle_stream_data(request);
        case protocol::Command::StreamCredit:
            return handle_stream_credit(request);
        case protocol::Command::StreamStatus:
            return handle_stream_status(request);
        case protocol::Command::StreamStop:
            return handle_stream_stop(request);
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
        case protocol::Command::MotionContract:
            return handle_motion_contract(request);
        case protocol::Command::MotionGroupPrepare:
            return handle_motion_group_prepare(request, now);
        case protocol::Command::MotionGroupCommit:
            return handle_motion_group_commit(request, now);
        case protocol::Command::MotionGroupAbort:
            return handle_motion_group_abort(request, now);
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
               }) ||
           std::any_of(
               stream_sessions_.begin(), stream_sessions_.end(),
               [resource_id](const auto& entry) {
                   return entry.second.resource_id == resource_id &&
                          entry.second.state !=
                              protocol::StreamState::Stopped;
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

bool RemoteCore::session_has_lease(
    std::uint32_t resource_id, std::uint32_t session_id) const noexcept {
    const auto found = leases_.find(resource_id);
    if (found == leases_.end()) {
        return false;
    }
    return std::any_of(
        found->second.begin(), found->second.end(),
        [session_id](const auto& lease) {
            return lease.owner_session_id == session_id;
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

bool RemoteCore::release_resource_objects(
    std::uint32_t resource_id, std::uint32_t owner_session_id) {
    bool all_released = true;
    const auto* released_resource = find_resource(resource_id);
    bool group_aborted = false;
    if (released_resource != nullptr &&
        released_resource->type ==
            protocol::ResourceType::StepgenAxis &&
        motion_group_ != nullptr &&
        motion_group_->owned_by(owner_session_id) &&
        motion_group_->uses_resource(resource_id)) {
        group_aborted = motion_group_->emergency_abort();
    }
    if (released_resource != nullptr &&
        released_resource->type ==
            protocol::ResourceType::StepgenAxis &&
        motion_ != nullptr && !group_aborted) {
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
                // 未确认安全低电平时保留对象及所有权，租约调用方据此保留租约。
                all_released = false;
                ++iterator;
                continue;
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

    for (auto& entry : stream_sessions_) {
        auto& stream = entry.second;
        if (stream.resource_id == resource_id &&
            stream.owner_session_id == owner_session_id &&
            stream.state != protocol::StreamState::Stopped) {
            stream.state = protocol::StreamState::Stopped;
            stream.available_credit = 0U;
            stream.outstanding_begin = 0U;
            stream.outstanding_count = 0U;
            if (stream_bsp_) {
                stream_bsp_->reset(resource_id);
            }
        }
    }
    return all_released;
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
            if (!release_resource_objects(
                    resource_id, iterator->owner_session_id)) {
                ++iterator;
                continue;
            }
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
    if (motion_group_ != nullptr) {
        static_cast<void>(motion_group_->cancel_session(session_id));
    }
    for (auto map_iterator = leases_.begin();
         map_iterator != leases_.end();) {
        const auto resource_id = map_iterator->first;
        auto& entries = map_iterator->second;
        for (auto iterator = entries.begin(); iterator != entries.end();) {
            if (iterator->owner_session_id != session_id) {
                ++iterator;
                continue;
            }
            if (!release_resource_objects(resource_id, session_id)) {
                ++iterator;
                continue;
            }
            iterator = entries.erase(iterator);
            ++released;
        }
        if (entries.empty()) {
            map_iterator = leases_.erase(map_iterator);
        } else {
            ++map_iterator;
        }
    }
    // 无需 ResourceAcquire 的 GPIO 仍属于创建会话；会话断开时也要安全清理。
    for (auto iterator = gpio_objects_.begin();
         iterator != gpio_objects_.end();) {
        const auto object = iterator->second;
        if (object.owner_session_id != session_id ||
            session_has_lease(object.resource_id, session_id)) {
            ++iterator;
            continue;
        }
        if (gpio_bsp_ && object.direction == GpioDirection::Output) {
            try {
                gpio_bsp_->write(object.pin, false);
            } catch (const std::exception&) {
                ++iterator;
                continue;
            }
        }
        iterator = gpio_objects_.erase(iterator);
        ++released;
    }
    if (parameter_unlock_session_ == session_id) {
        parameter_unlock_session_ = 0U;
        parameter_unlock_token_ = 0U;
        parameter_unlock_expires_ = TimePoint{};
    }
    for (auto& entry : stream_sessions_) {
        auto& stream = entry.second;
        if (stream.owner_session_id == session_id &&
            stream.state != protocol::StreamState::Stopped) {
            stream.state = protocol::StreamState::Stopped;
            stream.available_credit = 0U;
            stream.outstanding_begin = 0U;
            stream.outstanding_count = 0U;
            if (stream_bsp_) {
                stream_bsp_->reset(stream.resource_id);
            }
            ++released;
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

protocol::Packet RemoteCore::handle_time_sync(
    const protocol::Packet& request, TimePoint now) const {
    try {
        static_cast<void>(
            protocol::decode_time_sync_request(request.payload));
    } catch (const protocol::TimeSyncPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }

    std::vector<std::uint8_t> encoded;
    try {
        const auto capture = time_sync_bsp_->capture(now);
        encoded = protocol::encode_time_sync_response(
            {protocol::kTimeSyncPayloadVersion,
             capture.counter_bits,
             0U,
             capture.boot_epoch,
             capture.nominal_tick_rate_hz,
             capture.node_receive_tick,
             capture.node_send_tick});
    } catch (const std::exception&) {
        return make_response(request, StatusCode::ResourceFailed);
    }
    auto response = make_response(request, StatusCode::Ok);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
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
    } else if (found->type == protocol::ResourceType::Stream &&
               stream_bsp_) {
        const auto buffered = stream_bsp_->buffered_bytes(resource_id);
        const auto* stream_contract = stream_bsp_->contract(resource_id);
        const auto bounded = static_cast<std::uint32_t>(
            std::min<std::size_t>(buffered,
                                  std::numeric_limits<std::uint32_t>::max()));
        const auto dropped = stream_bsp_->dropped_bytes(resource_id);
        if (stream_contract != nullptr && stream_contract->direction ==
                protocol::StreamDirection::NodeToHost) {
            status.tx_buffered = bounded;
            status.tx_overruns = dropped;
            if (dropped != 0U) {
                status.error_flags |= protocol::kResourceErrorTxOverflow;
                status.health = protocol::ResourceHealth::Degraded;
            }
            if (found->tx_capacity != 0U &&
                static_cast<std::uint64_t>(buffered) * 4U >=
                    static_cast<std::uint64_t>(found->tx_capacity) * 3U) {
                status.error_flags |= protocol::kResourceErrorTxHighWater;
                if (status.health == protocol::ResourceHealth::Normal) {
                    status.health = protocol::ResourceHealth::Busy;
                }
            }
        } else {
            status.rx_buffered = bounded;
            status.rx_overruns = dropped;
            if (dropped != 0U) {
                status.error_flags |= protocol::kResourceErrorRxOverflow;
                status.health = protocol::ResourceHealth::Degraded;
            }
            if (found->rx_capacity != 0U &&
                static_cast<std::uint64_t>(buffered) * 4U >=
                    static_cast<std::uint64_t>(found->rx_capacity) * 3U) {
                status.error_flags |= protocol::kResourceErrorRxHighWater;
                if (status.health == protocol::ResourceHealth::Normal) {
                    status.health = protocol::ResourceHealth::Busy;
                }
            }
        }
        const bool failed = std::any_of(
            stream_sessions_.begin(), stream_sessions_.end(),
            [resource_id](const auto& entry) {
                return entry.second.resource_id == resource_id &&
                       entry.second.state == protocol::StreamState::Failed;
            });
        if (failed) {
            status.error_flags |= protocol::kResourceErrorBackendFailure;
            status.health = protocol::ResourceHealth::Failed;
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
        release_resource_objects(resource_id, 0U);
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
        release_resource_objects(resource_id, 0U);
        return make_response(request, StatusCode::Ok);
    }
    if (found->type == protocol::ResourceType::Stream && stream_bsp_) {
        stream_bsp_->reset(resource_id);
        for (auto& entry : stream_sessions_) {
            if (entry.second.resource_id == resource_id) {
                entry.second.state = protocol::StreamState::Stopped;
                entry.second.available_credit = 0U;
                entry.second.outstanding_begin = 0U;
                entry.second.outstanding_count = 0U;
            }
        }
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
    release_resource_objects(resource_id, request.header.session_id);
    release_resource_objects(resource_id, 0U);
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
    if (!release_resource_objects(release.resource_id,
                                  request.header.session_id)) {
        return make_response(request, StatusCode::ResourceFailed);
    }
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

protocol::Packet RemoteCore::handle_device_parameter_status(
    const protocol::Packet& request, TimePoint now) const {
    if (!device_parameters_ ||
        (capabilities_ & capability_mask(Capability::DeviceParameters)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U || !request.payload.empty()) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::DeviceParameterStatus status;
    status.generation = device_parameters_->generation();
    status.stored_count = device_parameters_->stored_count();
    status.definition_count = static_cast<std::uint16_t>(
        rbsp_device_param_definition_count());
    status.last_error = device_parameters_->last_error();
    if (parameter_unlock_session_ == request.header.session_id &&
        parameter_unlock_token_ != 0U && now < parameter_unlock_expires_) {
        status.flags |= static_cast<std::uint16_t>(
            protocol::DeviceParameterStatusFlag::MaintenanceUnlocked);
    }
    if (parameter_restart_required_) {
        status.flags |= static_cast<std::uint16_t>(
            protocol::DeviceParameterStatusFlag::RestartRequired);
    }
    auto response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_device_parameter_status(status);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_device_parameter_list(
    const protocol::Packet& request) const {
    if (!device_parameters_ ||
        (capabilities_ & capability_mask(Capability::DeviceParameters)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U || !request.payload.empty()) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    std::vector<protocol::DeviceParameterDescriptor> descriptors;
    descriptors.reserve(rbsp_device_param_definition_count());
    for (std::size_t index = 0U;
         index < rbsp_device_param_definition_count(); ++index) {
        rbsp_device_param_definition definition;
        if (!rbsp_device_param_definition_at(index, &definition)) {
            return make_response(request, StatusCode::ResourceFailed);
        }
        descriptors.push_back({definition.id, definition.type,
                               definition.flags,
                               definition.minimum_length,
                               definition.maximum_length});
    }
    auto response = make_response(request, StatusCode::Ok);
    const auto encoded =
        protocol::encode_device_parameter_descriptors(descriptors);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_device_parameter_read(
    const protocol::Packet& request) const {
    if (!device_parameters_ ||
        (capabilities_ & capability_mask(Capability::DeviceParameters)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    std::uint16_t id;
    try {
        id = protocol::decode_device_parameter_read_request(request.payload);
    } catch (const std::invalid_argument&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    rbsp_device_param_record record;
    if (!device_parameters_->read(id, record)) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    protocol::DeviceParameterValue value;
    value.id = record.id;
    value.type = record.type;
    value.flags = record.flags;
    value.generation = device_parameters_->generation();
    value.value.assign(record.value, record.value + record.length);
    auto response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_device_parameter_value(value);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_device_parameter_unlock(
    const protocol::Packet& request, TimePoint now) {
    if (!device_parameters_ ||
        (capabilities_ & capability_mask(Capability::DeviceParameters)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::DeviceParameterUnlockRequest unlock;
    try {
        unlock = protocol::decode_device_parameter_unlock_request(
            request.payload);
    } catch (const std::invalid_argument&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (unlock.confirmation !=
            protocol::kDeviceParameterUnlockConfirmation ||
        unlock.expected_generation != device_parameters_->generation() ||
        request.header.session_id == 0U) {
        return make_response(request, StatusCode::AccessDenied);
    }
    parameter_unlock_session_ = request.header.session_id;
    parameter_unlock_token_ = 0xD15EA5E5U ^ request.header.session_id ^
                              device_parameters_->generation();
    if (parameter_unlock_token_ == 0U) {
        parameter_unlock_token_ = 1U;
    }
    parameter_unlock_expires_ = now + std::chrono::seconds(60);
    auto response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_device_parameter_unlock_response(
        {device_parameters_->generation(), parameter_unlock_token_});
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_device_parameter_write(
    const protocol::Packet& request, TimePoint now) {
    if (!device_parameters_ ||
        (capabilities_ & capability_mask(Capability::DeviceParameters)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U ||
        parameter_unlock_session_ != request.header.session_id ||
        parameter_unlock_token_ == 0U || now >= parameter_unlock_expires_) {
        return make_response(request, StatusCode::AccessDenied);
    }
    protocol::DeviceParameterWriteRequest write;
    try {
        write = protocol::decode_device_parameter_write_request(
            request.payload);
    } catch (const std::invalid_argument&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (write.token != parameter_unlock_token_) {
        return make_response(request, StatusCode::AccessDenied);
    }
    rbsp_device_param_definition definition;
    if (!rbsp_device_param_find_definition(write.id, &definition) ||
        !rbsp_device_param_validate_value(
            write.id, write.value.empty() ? nullptr : write.value.data(),
            write.value.size())) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (!device_parameters_->write(
            write.expected_generation, write.id, write.value)) {
        return make_response(
            request,
            device_parameters_->last_error() ==
                    RBSP_DEVICE_PARAM_STORE_ERROR_WRITE_ONCE
                ? StatusCode::AccessDenied
                : StatusCode::ResourceFailed);
    }
    if ((definition.flags &
         RBSP_DEVICE_PARAM_FLAG_APPLY_AFTER_RESTART) != 0U) {
        parameter_restart_required_ = true;
    }
    auto response = make_response(request, StatusCode::Ok);
    protocol::DeviceParameterStatus status;
    status.generation = device_parameters_->generation();
    status.stored_count = device_parameters_->stored_count();
    status.definition_count = static_cast<std::uint16_t>(
        rbsp_device_param_definition_count());
    status.last_error = device_parameters_->last_error();
    status.flags = static_cast<std::uint16_t>(
        protocol::DeviceParameterStatusFlag::MaintenanceUnlocked);
    if (parameter_restart_required_) {
        status.flags |= static_cast<std::uint16_t>(
            protocol::DeviceParameterStatusFlag::RestartRequired);
    }
    const auto encoded = protocol::encode_device_parameter_status(status);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_device_parameter_lock(
    const protocol::Packet& request) {
    if (!device_parameters_ ||
        (capabilities_ & capability_mask(Capability::DeviceParameters)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U || !request.payload.empty()) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (parameter_unlock_session_ == request.header.session_id) {
        parameter_unlock_session_ = 0U;
        parameter_unlock_token_ = 0U;
        parameter_unlock_expires_ = TimePoint{};
    }
    return make_response(request, StatusCode::Ok);
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
    bool initial_value = request.payload[3] != 0;
    const bool catalog_has_gpio = std::any_of(
        resources_.begin(), resources_.end(), [](const auto& resource) {
            return resource.type == protocol::ResourceType::Gpio;
        });
    const auto* resource =
        find_resource(protocol::ResourceType::Gpio, pin);
    if (catalog_has_gpio && resource == nullptr) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    const std::uint32_t owner_session_id = request.header.session_id;
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
    if (found->second.owner_session_id != request.header.session_id) {
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
    if (found->second.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    if (found->second.direction != GpioDirection::Output) {
        return make_response(request, StatusCode::AccessDenied);
    }
    gpio_bsp_->write(found->second.pin, request.payload[0] != 0);
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::handle_gpio_close(
    const protocol::Packet& request) {
    if (!gpio_bsp_ ||
        (capabilities_ & capability_mask(Capability::Gpio)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id == 0U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    try {
        static_cast<void>(protocol::decode_gpio_close(request.payload));
    } catch (const protocol::GpioPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto found = gpio_objects_.find(request.header.object_id);
    if (found == gpio_objects_.end()) {
        // 不存在即“已经关闭”。这使响应丢失后的同命令重试仍能确定成功，
        // 同时不需要保存无界 tombstone。
        return make_response(request, StatusCode::Ok);
    }
    if (found->second.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    if (found->second.direction == GpioDirection::Output) {
        try {
            // GPIO_CLOSE 自身必须失效安全，不能假设调用方已经先写低。
            gpio_bsp_->write(found->second.pin, false);
        } catch (const std::exception&) {
            // 写低未得到确定成功时保留对象，允许同一所有者重试。
            return make_response(request, StatusCode::ResourceFailed);
        }
    }
    gpio_objects_.erase(found);
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

std::vector<protocol::Packet> RemoteCore::poll_stream_events(
    std::size_t maximum_events, TimePoint now) {
    auto prepared = prepare_stream_events(maximum_events, now);
    std::vector<protocol::Packet> events;
    events.reserve(prepared.size());
    for (auto& event : prepared) {
        events.push_back(std::move(event.packet_));
        if (!commit_stream_event(event, now)) {
            events.pop_back();
        }
    }
    return events;
}

std::vector<RemoteCore::PreparedStreamEvent>
RemoteCore::prepare_stream_events(
    std::size_t maximum_events, TimePoint now) {
    if (maximum_events == 0U || maximum_events > kMaximumStreamSessions) {
        throw std::invalid_argument("STREAM 单次事件数量无效");
    }
    static_cast<void>(expire_leases(now));
    std::vector<PreparedStreamEvent> events;
    if (!stream_bsp_) {
        return events;
    }
    events.reserve(maximum_events);
    std::vector<std::uint32_t> stream_ids;
    stream_ids.reserve(stream_sessions_.size());
    for (const auto& entry : stream_sessions_) {
        stream_ids.push_back(entry.first);
    }
    std::sort(stream_ids.begin(), stream_ids.end());
    for (const auto stream_id : stream_ids) {
        if (events.size() >= maximum_events) {
            break;
        }
        auto found = stream_sessions_.find(stream_id);
        if (found == stream_sessions_.end()) {
            continue;
        }
        auto& stream = found->second;
        if (stream.direction != protocol::StreamDirection::NodeToHost ||
            stream.state == protocol::StreamState::Stopped ||
            stream.state == protocol::StreamState::Failed) {
            continue;
        }
        const auto* contract = stream_bsp_->contract(stream.resource_id);
        std::uint32_t buffered = 0U;
        std::uint32_t unused_capacity = 0U;
        if (contract == nullptr || contract->direction != stream.direction ||
            !stream_capacity(*stream_bsp_, *contract, stream.resource_id,
                             buffered, unused_capacity)) {
            stream.state = protocol::StreamState::Failed;
            continue;
        }
        if (buffered == 0U) {
            continue;
        }
        if (stream.available_credit == 0U ||
            stream.outstanding_count == stream.outstanding.size()) {
            stream.state = protocol::StreamState::Backpressured;
            continue;
        }
        if (stream.next_sequence ==
                std::numeric_limits<std::uint32_t>::max()) {
            stream.state = protocol::StreamState::Failed;
            continue;
        }
        const auto maximum_bytes = std::min<std::uint32_t>(
            {stream.negotiated_chunk_bytes, stream.available_credit,
             buffered});
        const auto pulled = stream_bsp_->peek(
            stream.resource_id, maximum_bytes);
        if (pulled.status != StreamPushStatus::Accepted ||
            pulled.data.empty() || pulled.data.size() > maximum_bytes) {
            if (pulled.status == StreamPushStatus::Failed ||
                !pulled.data.empty() || pulled.data.size() > maximum_bytes) {
                stream.state = protocol::StreamState::Failed;
            }
            continue;
        }
        PreparedStreamEvent event;
        event.stream_id = stream_id;
        event.sequence = stream.next_sequence;
        event.bytes = static_cast<std::uint32_t>(pulled.data.size());
        event.packet_.header.message_type = protocol::MessageType::Event;
        event.packet_.header.command = static_cast<std::uint16_t>(
            protocol::Command::StreamData);
        event.packet_.header.session_id = stream.owner_session_id;
        event.packet_.header.request_id = event.sequence + 1U;
        event.packet_.header.object_id = stream.resource_id;
        event.packet_.payload = protocol::encode_stream_data(
            {stream_id, event.sequence, 0U, 0U, pulled.data});
        events.push_back(std::move(event));
    }
    return events;
}

bool RemoteCore::commit_stream_event(
    const PreparedStreamEvent& event, TimePoint now) noexcept {
    if (!stream_bsp_ || event.stream_id == 0U || event.bytes == 0U) {
        return false;
    }
    const auto found = stream_sessions_.find(event.stream_id);
    if (found == stream_sessions_.end()) {
        return false;
    }
    auto& stream = found->second;
    const auto leases = leases_.find(stream.resource_id);
    const bool lease_current = leases != leases_.end() && std::any_of(
        leases->second.begin(), leases->second.end(),
        [&](const auto& lease) {
            return lease.owner_session_id == stream.owner_session_id &&
                   lease.expires_at > now;
        });
    if (stream.direction != protocol::StreamDirection::NodeToHost ||
        stream.state == protocol::StreamState::Stopped ||
        stream.state == protocol::StreamState::Failed ||
        stream.next_sequence != event.sequence ||
        stream.next_sequence == std::numeric_limits<std::uint32_t>::max() ||
        event.bytes > stream.negotiated_chunk_bytes ||
        event.bytes > stream.available_credit ||
        stream.outstanding_count == stream.outstanding.size() ||
        !lease_current) {
        return false;
    }
    if (!stream_bsp_->commit_pull(stream.resource_id, event.bytes)) {
        stream.state = protocol::StreamState::Failed;
        return false;
    }
    const auto outstanding_index =
        (stream.outstanding_begin + stream.outstanding_count) %
        stream.outstanding.size();
    stream.outstanding[outstanding_index] = {event.sequence, event.bytes};
    ++stream.outstanding_count;
    stream.available_credit -= event.bytes;
    ++stream.next_sequence;
    stream.state = stream.available_credit == 0U &&
                           stream_bsp_->buffered_bytes(stream.resource_id) !=
                               0U
                       ? protocol::StreamState::Backpressured
                       : protocol::StreamState::Running;
    return true;
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

protocol::Packet RemoteCore::handle_i2c_contract(
    const protocol::Packet& request) const {
    if (!bus_bsp_ ||
        (capabilities_ & capability_mask(Capability::I2c)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0 || request.payload.size() != 4) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto resource_id = protocol::decode_resource_id(request.payload);
    const auto* resource = find_resource(resource_id);
    if (resource == nullptr ||
        resource->type != protocol::ResourceType::I2cDevice) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    const auto* contract = bus_bsp_->contract(resource_id);
    if (contract == nullptr ||
        contract->kind != protocol::BusResourceKind::I2cDevice) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_bus_resource_contract(*contract);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_i2c_transfer(
    const protocol::Packet& request) {
    if (!bus_bsp_ ||
        (capabilities_ & capability_mask(Capability::I2c)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::I2cTransferRequest transfer;
    try {
        transfer = protocol::decode_i2c_transfer_request(request.payload);
    } catch (const protocol::BusStreamPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto* resource = find_resource(transfer.device_resource_id);
    if (resource == nullptr ||
        resource->type != protocol::ResourceType::I2cDevice) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    if (!resource_access_allowed(resource->resource_id,
                                 request.header.session_id)) {
        return make_response(request, StatusCode::AccessDenied);
    }
    protocol::BusTransferResult result;
    try {
        result = bus_bsp_->i2c_transfer(transfer);
    } catch (const MockBusException&) {
        return make_response(request, StatusCode::ResourceFailed);
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_bus_transfer_result(result);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_spi_contract(
    const protocol::Packet& request) const {
    if (!bus_bsp_ ||
        (capabilities_ & capability_mask(Capability::Spi)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0 || request.payload.size() != 4) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto resource_id = protocol::decode_resource_id(request.payload);
    const auto* resource = find_resource(resource_id);
    if (resource == nullptr ||
        resource->type != protocol::ResourceType::SpiDevice) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    const auto* contract = bus_bsp_->contract(resource_id);
    if (contract == nullptr ||
        contract->kind != protocol::BusResourceKind::SpiDevice) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_bus_resource_contract(*contract);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_spi_transfer(
    const protocol::Packet& request) {
    if (!bus_bsp_ ||
        (capabilities_ & capability_mask(Capability::Spi)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::SpiTransferRequest transfer;
    try {
        transfer = protocol::decode_spi_transfer_request(request.payload);
    } catch (const protocol::BusStreamPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto* resource = find_resource(transfer.device_resource_id);
    if (resource == nullptr ||
        resource->type != protocol::ResourceType::SpiDevice) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    if (!resource_access_allowed(resource->resource_id,
                                 request.header.session_id)) {
        return make_response(request, StatusCode::AccessDenied);
    }
    protocol::BusTransferResult result;
    try {
        result = bus_bsp_->spi_transfer(transfer);
    } catch (const MockBusException&) {
        return make_response(request, StatusCode::ResourceFailed);
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_bus_transfer_result(result);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_stream_contract(
    const protocol::Packet& request) const {
    if (!stream_bsp_ ||
        (capabilities_ & capability_mask(Capability::Stream)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U || request.payload.size() != 4U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto resource_id = protocol::decode_resource_id(request.payload);
    const auto* resource = find_resource(resource_id);
    if (resource == nullptr || resource->type != protocol::ResourceType::Stream) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    const auto* contract = stream_bsp_->contract(resource_id);
    if (contract == nullptr) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    auto response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_stream_contract(*contract);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_stream_open(
    const protocol::Packet& request) {
    if (!stream_bsp_ ||
        (capabilities_ & capability_mask(Capability::Stream)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U || request.header.session_id == 0U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::StreamOpenRequest open;
    try {
        open = protocol::decode_stream_open_request(request.payload);
    } catch (const protocol::BusStreamPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto* resource = find_resource(open.resource_id);
    const auto* contract = stream_bsp_->contract(open.resource_id);
    if (resource == nullptr || resource->type != protocol::ResourceType::Stream ||
        contract == nullptr) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    // Mock 切片支持单向原始字节流；双向与时间戳仍不能静默降级。
    if (contract->direction == protocol::StreamDirection::Bidirectional ||
        (contract->flags & protocol::kStreamFlagTimestamped) != 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    const bool node_to_host = contract->direction ==
        protocol::StreamDirection::NodeToHost;
    if (open.requested_chunk_bytes > contract->maximum_chunk_bytes ||
        open.requested_flags != contract->flags) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if ((!node_to_host && open.initial_credit_bytes != 0U) ||
        (node_to_host &&
         (((contract->flags & protocol::kStreamFlagCreditRequired) == 0U) ||
          open.initial_credit_bytes == 0U ||
          open.initial_credit_bytes > contract->buffer_capacity_bytes))) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (!resource_access_allowed(open.resource_id,
                                 request.header.session_id)) {
        return make_response(request, StatusCode::AccessDenied);
    }
    const auto* resource_contract = find_contract(open.resource_id);
    constexpr std::uint16_t required_h2n_access =
        protocol::kResourceAccessWritable |
        protocol::kResourceAccessExclusiveWrite |
        protocol::kResourceAccessLeaseSupported |
        protocol::kResourceAccessLeaseRequired;
    constexpr std::uint16_t required_n2h_access =
        protocol::kResourceAccessReadable |
        protocol::kResourceAccessLeaseSupported |
        protocol::kResourceAccessLeaseRequired;
    const bool access_valid = resource_contract != nullptr &&
        ((!node_to_host &&
          (resource_contract->access_flags & required_h2n_access) ==
              required_h2n_access &&
          session_has_exclusive_lease(open.resource_id,
                                      request.header.session_id)) ||
         (node_to_host &&
          (resource_contract->access_flags & required_n2h_access) ==
              required_n2h_access &&
          session_has_lease(open.resource_id, request.header.session_id)));
    if (!access_valid) {
        return make_response(request, StatusCode::AccessDenied);
    }
    const bool active = std::any_of(
        stream_sessions_.begin(), stream_sessions_.end(),
        [&](const auto& entry) {
            return entry.second.resource_id == open.resource_id &&
                   entry.second.state != protocol::StreamState::Stopped;
        });
    if (active) {
        return make_response(request, StatusCode::ResourceBusy);
    }
    if (stream_sessions_.size() >= kMaximumStreamSessions) {
        const auto stopped = std::find_if(
            stream_sessions_.begin(), stream_sessions_.end(),
            [](const auto& entry) {
                return entry.second.state == protocol::StreamState::Stopped;
            });
        if (stopped == stream_sessions_.end()) {
            return make_response(request, StatusCode::ResourceExhausted);
        }
        stream_sessions_.erase(stopped);
    }
    if (next_stream_id_ == 0U) {
        return make_response(request, StatusCode::ResourceExhausted);
    }
    const auto stream_id = next_stream_id_++;
    if (node_to_host) {
        // 新 stream_id 是新的会话代次；打开时丢弃上一代残留字节和计数。
        stream_bsp_->reset(open.resource_id);
    }
    StreamSession session;
    session.resource_id = open.resource_id;
    session.owner_session_id = request.header.session_id;
    session.direction = contract->direction;
    session.negotiated_chunk_bytes = open.requested_chunk_bytes;
    session.negotiated_flags = open.requested_flags;
    session.credit_limit = node_to_host ? open.initial_credit_bytes : 0U;
    session.available_credit = session.credit_limit;
    stream_sessions_.emplace(stream_id, std::move(session));
    std::uint32_t buffered = 0U;
    std::uint32_t available = 0U;
    if (!stream_capacity(*stream_bsp_, *contract, open.resource_id,
                         buffered, available)) {
        stream_sessions_.erase(stream_id);
        return make_response(request, StatusCode::ResourceFailed);
    }
    auto response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_stream_open_response(
        {stream_id, open.requested_chunk_bytes, open.requested_flags,
         node_to_host ? open.initial_credit_bytes : available});
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_stream_data(
    const protocol::Packet& request) {
    if (!stream_bsp_ ||
        (capabilities_ & capability_mask(Capability::Stream)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::StreamDataPayload data;
    try {
        data = protocol::decode_stream_data(request.payload);
    } catch (const protocol::BusStreamPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto found = stream_sessions_.find(data.stream_id);
    if (found == stream_sessions_.end()) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    auto& stream = found->second;
    if (stream.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    if (stream.direction != protocol::StreamDirection::HostToNode) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (stream.state == protocol::StreamState::Stopped ||
        data.sequence != stream.next_sequence ||
        data.data.size() > stream.negotiated_chunk_bytes ||
        data.flags != 0U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto* contract = stream_bsp_->contract(stream.resource_id);
    if (contract == nullptr) {
        stream.state = protocol::StreamState::Failed;
        return make_response(request, StatusCode::ResourceFailed);
    }
    if (stream.next_sequence == std::numeric_limits<std::uint32_t>::max()) {
        stream.state = protocol::StreamState::Failed;
        return make_response(request, StatusCode::ResourceExhausted);
    }
    std::uint32_t buffered = 0U;
    std::uint32_t available = 0U;
    if (!stream_capacity(*stream_bsp_, *contract, stream.resource_id,
                         buffered, available)) {
        stream.state = protocol::StreamState::Failed;
        return make_response(request, StatusCode::ResourceFailed);
    }
    const auto push_status = data.data.size() > available
                                 ? StreamPushStatus::Backpressured
                                 : stream_bsp_->push(stream.resource_id,
                                                     data.data);
    if (push_status == StreamPushStatus::Failed) {
        stream.state = protocol::StreamState::Failed;
        return make_response(request, StatusCode::ResourceFailed);
    }
    if (push_status == StreamPushStatus::Backpressured) {
        if ((stream.negotiated_flags & protocol::kStreamFlagLossless) != 0U) {
            stream.state = protocol::StreamState::Backpressured;
            return make_response(request, StatusCode::ResourceBusy);
        }
        stream.dropped_bytes = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                static_cast<std::uint64_t>(stream.dropped_bytes) +
                    data.data.size(),
                std::numeric_limits<std::uint32_t>::max()));
    } else {
        stream.state = protocol::StreamState::Running;
    }
    ++stream.next_sequence;
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::handle_stream_credit(
    const protocol::Packet& request) {
    if (!stream_bsp_ ||
        (capabilities_ & capability_mask(Capability::Stream)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U || request.header.session_id == 0U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::StreamCreditPayload credit;
    try {
        credit = protocol::decode_stream_credit(request.payload);
    } catch (const protocol::BusStreamPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto found = stream_sessions_.find(credit.stream_id);
    if (found == stream_sessions_.end()) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    auto& stream = found->second;
    if (stream.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    if (stream.direction != protocol::StreamDirection::NodeToHost) {
        // H2N 中主机是发送方，无权给自身增发信用。
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (stream.state == protocol::StreamState::Stopped ||
        stream.state == protocol::StreamState::Failed) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    std::uint64_t acknowledged_bytes = 0U;
    std::size_t acknowledged_count = 0U;
    for (std::size_t offset = 0U; offset < stream.outstanding_count;
         ++offset) {
        const auto index = (stream.outstanding_begin + offset) %
            stream.outstanding.size();
        acknowledged_bytes += stream.outstanding[index].bytes;
        if (stream.outstanding[index].sequence ==
                credit.acknowledged_sequence) {
            acknowledged_count = offset + 1U;
            break;
        }
    }
    if (acknowledged_count == 0U ||
        acknowledged_bytes != credit.credit_bytes ||
        acknowledged_bytes > stream.credit_limit - stream.available_credit) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    stream.outstanding_begin =
        (stream.outstanding_begin + acknowledged_count) %
        stream.outstanding.size();
    stream.outstanding_count -= acknowledged_count;
    stream.available_credit += static_cast<std::uint32_t>(acknowledged_bytes);
    if (stream.state == protocol::StreamState::Backpressured) {
        stream.state = protocol::StreamState::Running;
    }
    return make_response(request, StatusCode::Ok);
}

protocol::Packet RemoteCore::handle_stream_status(
    const protocol::Packet& request) const {
    if (!stream_bsp_ ||
        (capabilities_ & capability_mask(Capability::Stream)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U || request.header.session_id == 0U ||
        request.payload.size() != 4U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto stream_id = protocol::decode_resource_id(request.payload);
    const auto found = stream_sessions_.find(stream_id);
    if (found == stream_sessions_.end()) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    const auto& stream = found->second;
    if (stream.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    const auto* contract = stream_bsp_->contract(stream.resource_id);
    if (contract == nullptr) {
        return make_response(request, StatusCode::ResourceFailed);
    }
    std::uint32_t buffered = 0U;
    std::uint32_t available = 0U;
    if (!stream_capacity(*stream_bsp_, *contract, stream.resource_id,
                         buffered, available)) {
        return make_response(request, StatusCode::ResourceFailed);
    }
    auto state = stream.state;
    if (stream.direction == protocol::StreamDirection::NodeToHost) {
        available = stream.available_credit;
    }
    if (state == protocol::StreamState::Backpressured && available != 0U) {
        if (stream.direction != protocol::StreamDirection::NodeToHost ||
            stream.outstanding_count < stream.outstanding.size()) {
            state = protocol::StreamState::Running;
        }
    }
    auto response = make_response(request, StatusCode::Ok);
    const auto dropped = stream.direction ==
            protocol::StreamDirection::NodeToHost
        ? stream_bsp_->dropped_bytes(stream.resource_id)
        : stream.dropped_bytes;
    const auto encoded = protocol::encode_stream_status(
        {stream_id, state, buffered,
         available, dropped, stream.next_sequence});
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_stream_stop(
    const protocol::Packet& request) {
    if (!stream_bsp_ ||
        (capabilities_ & capability_mask(Capability::Stream)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U || request.header.session_id == 0U ||
        request.payload.size() != 4U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const auto stream_id = protocol::decode_resource_id(request.payload);
    const auto found = stream_sessions_.find(stream_id);
    if (found == stream_sessions_.end()) {
        return make_response(request, StatusCode::ObjectNotFound);
    }
    if (found->second.owner_session_id != request.header.session_id) {
        return make_response(request, StatusCode::AccessDenied);
    }
    found->second.state = protocol::StreamState::Stopped;
    found->second.available_credit = 0U;
    found->second.outstanding_begin = 0U;
    found->second.outstanding_count = 0U;
    stream_bsp_->reset(found->second.resource_id);
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
    if (!motion_ || motion_->axes().empty() ||
        (capabilities_ & capability_mask(Capability::Motion)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (motion_group_ != nullptr &&
        (motion_group_->state() == MockMotionGroupState::Prepared ||
         motion_group_->state() == MockMotionGroupState::Armed)) {
        return make_response(request, StatusCode::ResourceBusy);
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
    if (!motion_ || motion_->axes().empty() ||
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

protocol::Packet RemoteCore::handle_motion_contract(
    const protocol::Packet& request) const {
    if (!motion_ || motion_->axes().empty() ||
        (capabilities_ & capability_mask(Capability::Motion)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0 || !request.payload.empty()) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::MotionContractPayload contract;
    contract.queue_capacity = static_cast<std::uint16_t>(
        motion_->status().queue_capacity);
    contract.minimum_lead_time_ns = motion_->minimum_lead_time_ns();
    contract.maximum_total_step_rate_hz =
        motion_->maximum_total_step_rate_hz();
    contract.axes.reserve(motion_->axes().size());
    for (const auto& axis : motion_->axes()) {
        contract.axes.push_back(
            {axis.resource_id, axis.maximum_step_rate_hz,
             axis.step_pulse_width_ns, axis.minimum_step_low_ns,
             axis.direction_setup_ns});
    }
    protocol::Packet response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_motion_contract(contract);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_motion_group_prepare(
    const protocol::Packet& request, TimePoint now) {
    if (motion_group_ == nullptr || motion_ == nullptr ||
        (capabilities_ & capability_mask(Capability::Motion)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::MotionGroupPreparePayload decoded;
    try {
        decoded = protocol::decode_motion_group_prepare(request.payload);
    } catch (const protocol::MotionGroupPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    for (const auto& axis : decoded.segment.axes) {
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
    const auto ready = motion_group_->prepare(
        decoded, protocol::MotionGroupReadyCode::Ready,
        request.header.session_id, now);
    auto response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_motion_group_ready(ready);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_motion_group_commit(
    const protocol::Packet& request, TimePoint now) {
    if (motion_group_ == nullptr || motion_ == nullptr ||
        (capabilities_ & capability_mask(Capability::Motion)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::MotionGroupCommitPayload decoded;
    try {
        decoded = protocol::decode_motion_group_commit(request.payload);
    } catch (const protocol::MotionGroupPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (motion_group_->owned_by(request.header.session_id) &&
        motion_group_->prepared().has_value()) {
        for (const auto& axis :
             motion_group_->prepared()->segment.axes) {
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
                static_cast<void>(motion_group_->emergency_abort());
                auto response = make_response(request, StatusCode::Ok);
                const auto encoded =
                    protocol::encode_motion_group_commit_ack(
                        {decoded.identity,
                         protocol::MotionGroupCommitCode::Rejected});
                response.payload.insert(response.payload.end(),
                                        encoded.begin(), encoded.end());
                return response;
            }
        }
    }
    const auto ack = motion_group_->commit(
        decoded, protocol::MotionGroupCommitCode::Armed,
        request.header.session_id, now);
    auto response = make_response(request, StatusCode::Ok);
    const auto encoded = protocol::encode_motion_group_commit_ack(ack);
    response.payload.insert(response.payload.end(), encoded.begin(),
                            encoded.end());
    return response;
}

protocol::Packet RemoteCore::handle_motion_group_abort(
    const protocol::Packet& request, TimePoint now) {
    if (motion_group_ == nullptr || motion_ == nullptr ||
        (capabilities_ & capability_mask(Capability::Motion)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0U) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    protocol::MotionGroupAbortPayload decoded;
    try {
        decoded = protocol::decode_motion_group_abort(request.payload);
    } catch (const protocol::MotionGroupPayloadException&) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    return make_response(
        request,
        motion_group_->abort(decoded, request.header.session_id, now)
            ? StatusCode::Ok
            : StatusCode::AccessDenied);
}

protocol::Packet RemoteCore::handle_motion_abort(
    const protocol::Packet& request) {
    if (!motion_ || motion_->axes().empty() ||
        (capabilities_ & capability_mask(Capability::Motion)) == 0U) {
        return make_response(request, StatusCode::UnsupportedCapability);
    }
    if (request.header.object_id != 0 || !request.payload.empty()) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    if (motion_group_ != nullptr && motion_group_->emergency_abort()) {
        // 普通 MOTION_ABORT 的既有契约要求锁存 Aborted。Prepared 事务尚未
        // 入队，参与者失效后仍需显式驱动执行器进入同一故障状态。
        if (motion_->status().fault == MotionFault::None) {
            try {
                static_cast<void>(motion_->abort(
                    motion_->status().node_time_ns,
                    MotionFault::Aborted));
            } catch (const MotionException&) {
                return make_response(request, StatusCode::ResourceFailed);
            }
        }
        return make_response(request, StatusCode::Ok);
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
    if (!motion_ || motion_->axes().empty() ||
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
    if (motion_group_ != nullptr) {
        motion_group_->reset();
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
