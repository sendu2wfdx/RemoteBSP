#include "remotebsp/mock_mcu/remote_core.hpp"

#include <algorithm>
#include <limits>

namespace remotebsp::mock_mcu {
namespace {

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
                       std::vector<protocol::ResourceDescriptor> resources)
    : node_info_(node_info),
      capabilities_(capabilities),
      gpio_bsp_(std::move(gpio_bsp)),
      uart_bsp_(std::move(uart_bsp)),
      resources_(std::move(resources)) {
    if (node_info_.protocol_version != protocol::kProtocolVersion) {
        throw CoreException(CoreError::UnsupportedVersion,
                            "节点协议版本与远程核心不兼容");
    }
}

protocol::Packet RemoteCore::handle(const protocol::Packet& request) {
    if (request.header.message_type != protocol::MessageType::Request) {
        throw CoreException(CoreError::NotRequest,
                            "远程核心只接受请求消息");
    }
    if (request.header.version != protocol::kProtocolVersion) {
        throw CoreException(CoreError::UnsupportedVersion,
                            "请求协议版本不受支持");
    }

    switch (static_cast<protocol::Command>(request.header.command)) {
        case protocol::Command::GetInfo:
            return handle_get_info(request);
        case protocol::Command::GetCapability:
            return handle_get_capability(request);
        case protocol::Command::Ping:
            return handle_ping(request);
        case protocol::Command::ResourceEnum:
            return handle_resource_enum(request);
        case protocol::Command::ResourceDescribe:
            return handle_resource_describe(request);
        case protocol::Command::ResourceStatus:
            return handle_resource_status(request);
        case protocol::Command::ResourceReset:
            return handle_resource_reset(request);
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
        default:
            return make_response(request, StatusCode::UnknownCommand);
    }
}

const NodeInfo& RemoteCore::node_info() const noexcept { return node_info_; }

std::uint64_t RemoteCore::capabilities() const noexcept {
    return capabilities_;
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
    const auto gpio_in_use = std::find_if(
        gpio_objects_.begin(), gpio_objects_.end(),
        [pin](const auto& entry) { return entry.second.pin == pin; });
    if (gpio_in_use != gpio_objects_.end()) {
        return make_response(request, StatusCode::ResourceBusy);
    }
    gpio_bsp_->configure(pin, direction, initial_value);

    const std::uint32_t object_id = next_object_id_++;
    gpio_objects_.emplace(object_id, GpioObject{pin, direction});
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
    if (request.header.object_id != 0 || request.payload.size() != 8) {
        return make_response(request, StatusCode::InvalidPayload);
    }
    const std::uint32_t baud_rate = read_u32(request.payload.data() + 1);
    const std::uint8_t data_bits = request.payload[5];
    const std::uint8_t stop_bits = request.payload[6];
    const std::uint8_t parity = request.payload[7];
    if (baud_rate == 0 || data_bits < 5 || data_bits > 8 ||
        (stop_bits != 1 && stop_bits != 2) ||
        parity > static_cast<std::uint8_t>(UartParity::Even)) {
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
    const bool port_is_exposed = std::any_of(
        resources_.begin(), resources_.end(),
        [port](const auto& resource) {
            return resource.type == protocol::ResourceType::Uart &&
                   resource.instance == port;
        });
    if (catalog_has_uart && !port_is_exposed) {
        return make_response(request, StatusCode::ObjectNotFound);
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
    uart_objects_.emplace(object_id, UartObject{port});
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
    try {
        uart_bsp_->write(found->second.port, request.payload);
    } catch (const std::exception& error) {
        return make_uart_error_response(request, error);
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
