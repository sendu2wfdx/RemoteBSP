#include "remotebsp/mock_mcu/remote_core.hpp"
#include "remotebsp/mock_mcu/stream_bsp.hpp"
#include "remotebsp/protocol/bus_stream.hpp"
#include "remotebsp/protocol/resource.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using namespace remotebsp;

protocol::Packet request(protocol::Command command,
                         std::vector<std::uint8_t> payload,
                         std::uint32_t session_id = 11U) {
    protocol::Packet packet;
    packet.header.message_type = protocol::MessageType::Request;
    packet.header.command = static_cast<std::uint16_t>(command);
    packet.header.session_id = session_id;
    packet.header.request_id = 1U;
    packet.payload = std::move(payload);
    return packet;
}

mock_mcu::StatusCode status(const protocol::Packet& response) {
    assert(!response.payload.empty());
    return static_cast<mock_mcu::StatusCode>(response.payload.front());
}

std::vector<std::uint8_t> body(const protocol::Packet& response) {
    assert(status(response) == mock_mcu::StatusCode::Ok);
    return {response.payload.begin() + 1, response.payload.end()};
}

}  // namespace

int main() {
    constexpr std::uint32_t resource_id = 0x0F000001U;
    const protocol::StreamContract stream_contract{
        resource_id, protocol::kStreamContractVersion,
        protocol::StreamDirection::HostToNode,
        protocol::kStreamTransportUsb,
        static_cast<std::uint16_t>(protocol::kStreamFlagLossless |
                                   protocol::kStreamFlagCreditRequired),
        4U, 8U, 100000U, 200000U, 1000U, 100U};
    const protocol::ResourceContract resource_contract{
        resource_id, protocol::kResourceContractVersion,
        static_cast<std::uint16_t>(
            protocol::kResourceAccessWritable |
            protocol::kResourceAccessExclusiveWrite |
            protocol::kResourceAccessLeaseSupported |
            protocol::kResourceAccessLeaseRequired),
        0U, 1000U, 1000U, 2U, 0U, 200000U};
    auto stream = std::make_shared<mock_mcu::MockStreamBsp>();
    stream->add_resource(stream_contract);
    mock_mcu::RemoteCore core(
        {}, mock_mcu::capability_mask(mock_mcu::Capability::Stream),
        nullptr, nullptr,
        {{resource_id, protocol::ResourceType::Stream, 1U,
          protocol::kResourceFlagNative, 8U, 0U}},
        {resource_contract}, nullptr, nullptr, nullptr, nullptr, nullptr,
        stream);

    const auto remote_contract = protocol::decode_stream_contract(body(
        core.handle(request(protocol::Command::StreamContract,
                            protocol::encode_resource_id(resource_id)))));
    assert(remote_contract.buffer_capacity_bytes == 8U);

    const protocol::StreamOpenRequest open{
        resource_id, 4U,
        static_cast<std::uint16_t>(protocol::kStreamFlagLossless |
                                   protocol::kStreamFlagCreditRequired),
        0U};
    assert(status(core.handle(request(
               protocol::Command::StreamOpen,
               protocol::encode_stream_open_request(open)))) ==
           mock_mcu::StatusCode::AccessDenied);

    body(core.handle(request(
        protocol::Command::ResourceAcquire,
        protocol::encode_resource_lease_request(
            {resource_id, 1000U,
             protocol::ResourceLeaseMode::Exclusive}))));
    const auto opened = protocol::decode_stream_open_response(body(
        core.handle(request(protocol::Command::StreamOpen,
                            protocol::encode_stream_open_request(open)))));
    assert(opened.available_credit_bytes == 8U);

    assert(status(core.handle(request(
               protocol::Command::StreamData,
               protocol::encode_stream_data(
                   {opened.stream_id, 0U, 0U, 0U, {1U}}),
               22U))) == mock_mcu::StatusCode::AccessDenied);
    body(core.handle(request(
        protocol::Command::StreamData,
        protocol::encode_stream_data(
            {opened.stream_id, 0U, 0U, 0U, {1U, 2U, 3U, 4U}}))));
    body(core.handle(request(
        protocol::Command::StreamData,
        protocol::encode_stream_data(
            {opened.stream_id, 1U, 0U, 0U, {5U, 6U, 7U, 8U}}))));

    // 无损流在缓冲已满时背压，序号不前进，重试不会形成重复写入。
    assert(status(core.handle(request(
               protocol::Command::StreamData,
               protocol::encode_stream_data(
                   {opened.stream_id, 2U, 0U, 0U, {9U}})))) ==
           mock_mcu::StatusCode::ResourceBusy);
    auto stream_status = protocol::decode_stream_status(body(core.handle(
        request(protocol::Command::StreamStatus,
                protocol::encode_resource_id(opened.stream_id)))));
    assert(stream_status.state == protocol::StreamState::Backpressured);
    assert(stream_status.buffered_bytes == 8U);
    assert(stream_status.available_credit_bytes == 0U);
    assert(stream_status.next_sequence == 2U);

    assert(stream->consume(resource_id, 4U) ==
           std::vector<std::uint8_t>({1U, 2U, 3U, 4U}));
    stream_status = protocol::decode_stream_status(body(core.handle(
        request(protocol::Command::StreamStatus,
                protocol::encode_resource_id(opened.stream_id)))));
    assert(stream_status.state == protocol::StreamState::Running);
    assert(stream_status.available_credit_bytes == 4U);
    body(core.handle(request(
        protocol::Command::StreamData,
        protocol::encode_stream_data(
            {opened.stream_id, 2U, 0U, 0U, {9U, 10U}}))));
    stream_status = protocol::decode_stream_status(body(core.handle(
        request(protocol::Command::StreamStatus,
                protocol::encode_resource_id(opened.stream_id)))));
    assert(stream_status.state == protocol::StreamState::Running);
    assert(stream_status.available_credit_bytes == 2U);
    assert(stream_status.next_sequence == 3U);

    // 已确认序号不能重放，且 H2N 发送方不能给自己增发信用。
    assert(status(core.handle(request(
               protocol::Command::StreamData,
               protocol::encode_stream_data(
                   {opened.stream_id, 2U, 0U, 0U, {9U}})))) ==
           mock_mcu::StatusCode::InvalidPayload);
    assert(status(core.handle(request(
               protocol::Command::StreamCredit,
               protocol::encode_stream_credit(
                   {opened.stream_id, 4U, 2U})))) ==
           mock_mcu::StatusCode::UnsupportedCapability);
    auto invalid_credit = request(
        protocol::Command::StreamCredit,
        protocol::encode_stream_credit({opened.stream_id, 4U, 2U}));
    invalid_credit.header.object_id = 1U;
    assert(status(core.handle(invalid_credit)) ==
           mock_mcu::StatusCode::InvalidPayload);

    // 独占租约阻止其他会话复位并破坏活动流。
    assert(status(core.handle(request(
               protocol::Command::ResourceReset,
               protocol::encode_resource_id(resource_id), 22U))) ==
           mock_mcu::StatusCode::AccessDenied);

    body(core.handle(request(protocol::Command::StreamStop,
                             protocol::encode_resource_id(opened.stream_id))));
    stream_status = protocol::decode_stream_status(body(core.handle(
        request(protocol::Command::StreamStatus,
                protocol::encode_resource_id(opened.stream_id)))));
    assert(stream_status.state == protocol::StreamState::Stopped);
    assert(stream_status.buffered_bytes == 0U);

    // 停止后可在同一租约内重开；旧会话字节不会泄漏到新会话。
    const auto reopened = protocol::decode_stream_open_response(body(
        core.handle(request(protocol::Command::StreamOpen,
                            protocol::encode_stream_open_request(open)))));
    assert(reopened.available_credit_bytes == 8U);
    body(core.handle(request(
        protocol::Command::StreamData,
        protocol::encode_stream_data(
            {reopened.stream_id, 0U, 0U, 0U, {42U}}))));
    assert(core.release_session(11U) == 1U);
    stream_status = protocol::decode_stream_status(body(core.handle(
        request(protocol::Command::StreamStatus,
                protocol::encode_resource_id(reopened.stream_id)))));
    assert(stream_status.state == protocol::StreamState::Stopped);
    assert(stream_status.buffered_bytes == 0U);

    return 0;
}
