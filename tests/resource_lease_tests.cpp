#include "remotebsp/mock_mcu/gpio_bsp.hpp"
#include "remotebsp/mock_mcu/mock_node.hpp"
#include "remotebsp/mock_mcu/remote_core.hpp"
#include "remotebsp/mock_mcu/uart_bsp.hpp"
#include "remotebsp/protocol/fragmentation.hpp"
#include "remotebsp/protocol/resource.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

using remotebsp::mock_mcu::RemoteCore;
using remotebsp::mock_mcu::StatusCode;
using remotebsp::protocol::Command;
using remotebsp::protocol::MessageType;
using remotebsp::protocol::Packet;
using remotebsp::protocol::ResourceContract;
using remotebsp::protocol::ResourceDescriptor;
using remotebsp::protocol::ResourceLeaseInfo;
using remotebsp::protocol::ResourceLeaseMode;
using remotebsp::protocol::ResourceLeaseRequest;
using remotebsp::protocol::ResourceLeaseTokenRequest;
using remotebsp::protocol::ResourceType;

constexpr std::uint32_t kGpioResourceId = 0x01000005;
constexpr std::uint32_t kUartResourceId = 0x02000002;

Packet request(Command command, std::uint32_t session_id,
               std::vector<std::uint8_t> payload = {},
               std::uint32_t object_id = 0) {
    Packet packet;
    packet.header.message_type = MessageType::Request;
    packet.header.command = static_cast<std::uint16_t>(command);
    packet.header.session_id = session_id;
    packet.header.request_id = 1;
    packet.header.object_id = object_id;
    packet.payload = std::move(payload);
    return packet;
}

void check_status(const Packet& response, StatusCode status) {
    assert(!response.payload.empty());
    assert(response.payload[0] == static_cast<std::uint8_t>(status));
}

std::vector<std::uint8_t> body(const Packet& response) {
    check_status(response, StatusCode::Ok);
    return {response.payload.begin() + 1, response.payload.end()};
}

RemoteCore make_core(
    const std::shared_ptr<remotebsp::mock_mcu::MockGpioBsp>& gpio,
    const std::shared_ptr<remotebsp::mock_mcu::MockUartBsp>& uart) {
    const std::vector<ResourceDescriptor> resources{
        {kGpioResourceId, ResourceType::Gpio, 5,
         remotebsp::protocol::kResourceFlagNative, 0, 0},
        {kUartResourceId, ResourceType::Uart, 2,
         remotebsp::protocol::kResourceFlagNative, 64, 64},
    };
    const std::vector<ResourceContract> contracts{
        {kGpioResourceId,
         remotebsp::protocol::kResourceContractVersion,
         static_cast<std::uint16_t>(
             remotebsp::protocol::kResourceAccessReadable |
             remotebsp::protocol::kResourceAccessWritable |
             remotebsp::protocol::kResourceAccessSharedRead |
             remotebsp::protocol::kResourceAccessExclusiveWrite |
             remotebsp::protocol::kResourceAccessLeaseSupported),
         1000, 100, 10000, 0, 0, 0},
        {kUartResourceId,
         remotebsp::protocol::kResourceContractVersion,
         static_cast<std::uint16_t>(
             remotebsp::protocol::kResourceAccessReadable |
             remotebsp::protocol::kResourceAccessWritable |
             remotebsp::protocol::kResourceAccessExclusiveWrite |
             remotebsp::protocol::kResourceAccessLeaseSupported),
         1000, 500, 5000, 64, 1000000, 1000000},
    };
    return RemoteCore(
        {},
        remotebsp::mock_mcu::capability_mask(
            remotebsp::mock_mcu::Capability::Gpio) |
            remotebsp::mock_mcu::capability_mask(
                remotebsp::mock_mcu::Capability::Uart),
        gpio, uart, resources, contracts);
}

ResourceLeaseInfo acquire(RemoteCore& core, std::uint32_t resource_id,
                          std::uint32_t session_id,
                          ResourceLeaseMode mode,
                          std::uint32_t duration_ms,
                          RemoteCore::TimePoint now) {
    const auto response = core.handle(
        request(
            Command::ResourceAcquire, session_id,
            remotebsp::protocol::encode_resource_lease_request(
                ResourceLeaseRequest{resource_id, duration_ms, mode})),
        now);
    return remotebsp::protocol::decode_resource_lease_info(body(response));
}

void test_exclusive_lease_and_safe_release() {
    auto gpio =
        std::make_shared<remotebsp::mock_mcu::MockGpioBsp>();
    auto uart =
        std::make_shared<remotebsp::mock_mcu::MockUartBsp>();
    auto core = make_core(gpio, uart);
    const auto start = RemoteCore::Clock::now();

    const auto lease = acquire(core, kUartResourceId, 11,
                               ResourceLeaseMode::Exclusive, 1000, start);
    assert(lease.lease_id != 0);
    assert(lease.owner_session_id == 11);
    assert(lease.active_lease_count == 1);

    const auto conflict = core.handle(
        request(
            Command::ResourceAcquire, 22,
            remotebsp::protocol::encode_resource_lease_request(
                {kUartResourceId, 1000,
                 ResourceLeaseMode::Exclusive})),
        start);
    check_status(conflict, StatusCode::ResourceBusy);

    auto create = request(Command::UartCreate, 11);
    create.payload = {2, 0x00, 0xC2, 0x01, 0x00, 8, 1, 0};
    const auto created = core.handle(create, start);
    check_status(created, StatusCode::Ok);
    assert(created.header.object_id != 0);

    auto denied_read =
        request(Command::UartRead, 22, {1, 0},
                created.header.object_id);
    check_status(core.handle(denied_read, start),
                 StatusCode::AccessDenied);

    const auto renewed = core.handle(
        request(
            Command::ResourceRenew, 11,
            remotebsp::protocol::encode_resource_lease_token_request(
                {kUartResourceId, lease.lease_id, 2000})),
        start + std::chrono::milliseconds(100));
    const auto renewed_info =
        remotebsp::protocol::decode_resource_lease_info(body(renewed));
    assert(renewed_info.granted_duration_ms == 2000);

    const auto wrong_release = core.handle(
        request(
            Command::ResourceRelease, 22,
            remotebsp::protocol::encode_resource_lease_token_request(
                {kUartResourceId, lease.lease_id, 0})),
        start);
    check_status(wrong_release, StatusCode::AccessDenied);

    const auto released = core.handle(
        request(
            Command::ResourceRelease, 11,
            remotebsp::protocol::encode_resource_lease_token_request(
                {kUartResourceId, lease.lease_id, 0})),
        start);
    check_status(released, StatusCode::Ok);
    check_status(core.handle(
                     request(Command::UartRead, 11, {1, 0},
                             created.header.object_id),
                     start),
                 StatusCode::ObjectNotFound);
}

void test_shared_read_and_session_release() {
    auto gpio =
        std::make_shared<remotebsp::mock_mcu::MockGpioBsp>();
    auto uart =
        std::make_shared<remotebsp::mock_mcu::MockUartBsp>();
    auto core = make_core(gpio, uart);
    const auto now = RemoteCore::Clock::now();

    const auto first = acquire(core, kGpioResourceId, 11,
                               ResourceLeaseMode::SharedRead, 1000, now);
    const auto second = acquire(core, kGpioResourceId, 22,
                                ResourceLeaseMode::SharedRead, 1000, now);
    assert(first.lease_id != second.lease_id);

    const auto status_response = core.handle(
        request(Command::ResourceLeaseStatus, 11,
                remotebsp::protocol::encode_resource_id(kGpioResourceId)),
        now);
    const auto status =
        remotebsp::protocol::decode_resource_lease_info(
            body(status_response));
    assert(status.active_lease_count == 2);
    assert(status.lease_id == 0);

    const auto exclusive = core.handle(
        request(
            Command::ResourceAcquire, 33,
            remotebsp::protocol::encode_resource_lease_request(
                {kGpioResourceId, 1000,
                 ResourceLeaseMode::Exclusive})),
        now);
    check_status(exclusive, StatusCode::ResourceBusy);

    assert(core.release_session(11) == 1);
    const auto after_release = core.handle(
        request(Command::ResourceLeaseStatus, 22,
                remotebsp::protocol::encode_resource_id(kGpioResourceId)),
        now);
    assert(remotebsp::protocol::decode_resource_lease_info(
               body(after_release))
               .active_lease_count == 1);
}

void test_expiry_forces_safe_gpio_state() {
    auto gpio =
        std::make_shared<remotebsp::mock_mcu::MockGpioBsp>();
    auto uart =
        std::make_shared<remotebsp::mock_mcu::MockUartBsp>();
    auto core = make_core(gpio, uart);
    const auto start = RemoteCore::Clock::now();

    acquire(core, kGpioResourceId, 44,
            ResourceLeaseMode::Exclusive, 100, start);
    auto create = request(Command::GpioCreate, 44);
    create.payload = {5, 0,
                      static_cast<std::uint8_t>(
                          remotebsp::mock_mcu::GpioDirection::Output),
                      1};
    const auto created = core.handle(create, start);
    check_status(created, StatusCode::Ok);
    assert(gpio->read(5));

    assert(core.expire_leases(
               start + std::chrono::milliseconds(101)) == 1);
    assert(!gpio->read(5));
    check_status(core.handle(
                     request(Command::GpioRead, 44, {},
                             created.header.object_id),
                     start + std::chrono::milliseconds(101)),
                 StatusCode::ObjectNotFound);

    const auto next = acquire(
        core, kGpioResourceId, 55,
        ResourceLeaseMode::Exclusive, 1000,
        start + std::chrono::milliseconds(101));
    assert(next.owner_session_id == 55);
}

std::optional<remotebsp::mock_mcu::NodeReply> send_request(
    remotebsp::mock_mcu::MockNode& node, const Packet& packet,
    std::uint16_t transfer_id) {
    const auto frames = remotebsp::protocol::Fragmenter(64).split(
        remotebsp::protocol::encode(packet), transfer_id);
    std::optional<remotebsp::mock_mcu::NodeReply> reply;
    for (const auto& frame : frames) {
        const auto current = node.handle_frame(0x600, frame);
        if (current.has_value()) {
            reply = current;
        }
    }
    return reply;
}

Packet decode_reply(const remotebsp::mock_mcu::NodeReply& reply) {
    remotebsp::protocol::Reassembler reassembler(
        64, std::chrono::milliseconds(100));
    remotebsp::protocol::ReassemblyResult result;
    for (const auto& frame : reply.frames) {
        result = reassembler.accept(0x580, frame);
    }
    assert(result.packet.has_value());
    return remotebsp::protocol::decode(*result.packet);
}

void test_duplicate_acquire_is_idempotent() {
    auto gpio =
        std::make_shared<remotebsp::mock_mcu::MockGpioBsp>();
    auto uart =
        std::make_shared<remotebsp::mock_mcu::MockUartBsp>();
    remotebsp::mock_mcu::MockNode node(
        make_core(gpio, uart), 64, 1);

    auto acquire_request = request(
        Command::ResourceAcquire, 77,
        remotebsp::protocol::encode_resource_lease_request(
            {kUartResourceId, 1000,
             ResourceLeaseMode::Exclusive}));
    acquire_request.header.request_id = 99;
    const auto first =
        decode_reply(*send_request(node, acquire_request, 1));
    const auto second =
        decode_reply(*send_request(node, acquire_request, 2));
    const auto first_info =
        remotebsp::protocol::decode_resource_lease_info(body(first));
    const auto second_info =
        remotebsp::protocol::decode_resource_lease_info(body(second));
    assert(first_info.lease_id == second_info.lease_id);
    assert(node.cached_response_count() == 1);

    auto status_request = request(
        Command::ResourceLeaseStatus, 77,
        remotebsp::protocol::encode_resource_id(kUartResourceId));
    status_request.header.request_id = 100;
    const auto status =
        decode_reply(*send_request(node, status_request, 3));
    assert(remotebsp::protocol::decode_resource_lease_info(body(status))
               .active_lease_count == 1);
}

void test_invalid_contract_catalog_is_rejected() {
    const std::vector<ResourceDescriptor> resources{
        {kGpioResourceId, ResourceType::Gpio, 5,
         remotebsp::protocol::kResourceFlagNative, 0, 0}};
    const ResourceContract invalid_contract{
        kUartResourceId,
        remotebsp::protocol::kResourceContractVersion,
        remotebsp::protocol::kResourceAccessLeaseSupported,
        0, 0, 0, 0, 0, 0};
    try {
        RemoteCore({}, 0, nullptr, nullptr, resources,
                   {invalid_contract});
        assert(false);
    } catch (const remotebsp::mock_mcu::CoreException& error) {
        assert(error.code() ==
               remotebsp::mock_mcu::CoreError::InvalidResourceCatalog);
    }
}

}

int main() {
    test_exclusive_lease_and_safe_release();
    test_shared_read_and_session_release();
    test_expiry_forces_safe_gpio_state();
    test_duplicate_acquire_is_idempotent();
    test_invalid_contract_catalog_is_rejected();
}
