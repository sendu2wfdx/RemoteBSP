#include "remotebsp/mock_mcu/mock_node.hpp"
#include "remotebsp/mock_mcu/remote_core.hpp"
#include "remotebsp/mock_mcu/stream_bsp.hpp"
#include "remotebsp/protocol/bus_stream.hpp"
#include "remotebsp/protocol/fragmentation.hpp"
#include "remotebsp/protocol/resource.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using namespace remotebsp;

constexpr std::uint32_t kLosslessResource = 0x0F000011U;
constexpr std::uint32_t kLossyResource = 0x0F000012U;

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

protocol::StreamContract stream_contract(std::uint32_t resource_id,
                                         bool lossless) {
    return {
        resource_id, protocol::kStreamContractVersion,
        protocol::StreamDirection::NodeToHost,
        protocol::kStreamTransportUsb,
        static_cast<std::uint16_t>(
            protocol::kStreamFlagCreditRequired |
            (lossless ? protocol::kStreamFlagLossless : 0U)),
        4U, lossless ? 8U : 4U, 100000U, 200000U, 1000U, 100U};
}

protocol::ResourceContract resource_contract(std::uint32_t resource_id) {
    return {
        resource_id, protocol::kResourceContractVersion,
        static_cast<std::uint16_t>(
            protocol::kResourceAccessReadable |
            protocol::kResourceAccessSharedRead |
            protocol::kResourceAccessLeaseSupported |
            protocol::kResourceAccessLeaseRequired),
        0U, 1000U, 1000U, 2U, 200000U, 0U};
}

mock_mcu::RemoteCore make_core(
    const std::shared_ptr<mock_mcu::MockStreamBsp>& stream,
    bool include_lossy = true) {
    stream->add_resource(stream_contract(kLosslessResource, true));
    std::vector<protocol::ResourceDescriptor> resources{
        {kLosslessResource, protocol::ResourceType::Stream, 1U,
         protocol::kResourceFlagNative, 0U, 8U}};
    std::vector<protocol::ResourceContract> contracts{
        resource_contract(kLosslessResource)};
    if (include_lossy) {
        stream->add_resource(stream_contract(kLossyResource, false));
        resources.push_back(
            {kLossyResource, protocol::ResourceType::Stream, 2U,
             protocol::kResourceFlagNative, 0U, 4U});
        contracts.push_back(resource_contract(kLossyResource));
    }
    return mock_mcu::RemoteCore(
        {}, mock_mcu::capability_mask(mock_mcu::Capability::Stream),
        nullptr, nullptr, std::move(resources), std::move(contracts),
        nullptr, nullptr, nullptr, nullptr, nullptr, stream);
}

void acquire(mock_mcu::RemoteCore& core, std::uint32_t resource_id,
             mock_mcu::RemoteCore::TimePoint now =
                 mock_mcu::RemoteCore::Clock::now(),
             std::uint32_t duration_ms = 1000U) {
    body(core.handle(request(
        protocol::Command::ResourceAcquire,
        protocol::encode_resource_lease_request(
            {resource_id, duration_ms,
             protocol::ResourceLeaseMode::SharedRead})), now));
}

protocol::StreamOpenResponse open(mock_mcu::RemoteCore& core,
                                  std::uint32_t resource_id,
                                  bool lossless,
                                  std::uint32_t credit = 6U,
                                  mock_mcu::RemoteCore::TimePoint now =
                                      mock_mcu::RemoteCore::Clock::now()) {
    return protocol::decode_stream_open_response(body(core.handle(
        request(protocol::Command::StreamOpen,
                protocol::encode_stream_open_request(
                    {resource_id, 4U,
                     static_cast<std::uint16_t>(
                         protocol::kStreamFlagCreditRequired |
                         (lossless ? protocol::kStreamFlagLossless : 0U)),
                     credit})), now)));
}

protocol::StreamStatusPayload stream_status(mock_mcu::RemoteCore& core,
                                            std::uint32_t stream_id) {
    return protocol::decode_stream_status(body(core.handle(request(
        protocol::Command::StreamStatus,
        protocol::encode_resource_id(stream_id)))));
}

void test_credit_sequence_generation_and_overflow_isolation() {
    auto stream = std::make_shared<mock_mcu::MockStreamBsp>();
    auto core = make_core(stream);
    acquire(core, kLosslessResource);
    acquire(core, kLossyResource);
    const auto first = open(core, kLosslessResource, true);
    const auto lossy = open(core, kLossyResource, false, 4U);
    assert(first.available_credit_bytes == 6U);

    assert(stream->produce(kLosslessResource,
                           {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}) ==
           mock_mcu::StreamPushStatus::Accepted);
    auto events = core.poll_stream_events();
    assert(events.size() == 1U);
    auto data = protocol::decode_stream_data(events.front().payload);
    assert(events.front().header.message_type == protocol::MessageType::Event);
    assert(events.front().header.session_id == 11U);
    assert(events.front().header.object_id == kLosslessResource);
    assert(data.stream_id == first.stream_id && data.sequence == 0U);
    assert(data.data == std::vector<std::uint8_t>({1U, 2U, 3U, 4U}));

    events = core.poll_stream_events();
    assert(events.size() == 1U);
    data = protocol::decode_stream_data(events.front().payload);
    assert(data.sequence == 1U);
    assert(data.data == std::vector<std::uint8_t>({5U, 6U}));
    auto current = stream_status(core, first.stream_id);
    assert(current.state == protocol::StreamState::Backpressured);
    assert(current.buffered_bytes == 2U);
    assert(current.available_credit_bytes == 0U);
    assert(current.next_sequence == 2U);

    assert(status(core.handle(request(
               protocol::Command::StreamCredit,
               protocol::encode_stream_credit(
                   {first.stream_id, 4U, 0U}), 22U))) ==
           mock_mcu::StatusCode::AccessDenied);

    body(core.handle(request(
        protocol::Command::StreamCredit,
        protocol::encode_stream_credit({first.stream_id, 4U, 0U}))));
    assert(status(core.handle(request(
               protocol::Command::StreamCredit,
               protocol::encode_stream_credit(
                   {first.stream_id, 4U, 0U})))) ==
           mock_mcu::StatusCode::InvalidPayload);
    events = core.poll_stream_events();
    assert(events.size() == 1U);
    data = protocol::decode_stream_data(events.front().payload);
    assert(data.sequence == 2U);
    assert(data.data == std::vector<std::uint8_t>({7U, 8U}));
    assert(status(core.handle(request(
               protocol::Command::StreamCredit,
               protocol::encode_stream_credit(
                   {first.stream_id, 3U, 2U})))) ==
           mock_mcu::StatusCode::InvalidPayload);
    body(core.handle(request(
        protocol::Command::StreamCredit,
        protocol::encode_stream_credit({first.stream_id, 4U, 2U}))));
    current = stream_status(core, first.stream_id);
    assert(current.available_credit_bytes == 6U);
    assert(current.next_sequence == 3U);

    // 有损资源整块丢弃溢出，不破坏另一资源会话。
    assert(stream->produce(kLossyResource, {9U, 10U, 11U, 12U}) ==
           mock_mcu::StreamPushStatus::Accepted);
    assert(stream->produce(kLossyResource, {13U, 14U, 15U}) ==
           mock_mcu::StreamPushStatus::Backpressured);
    auto lossy_status = stream_status(core, lossy.stream_id);
    assert(lossy_status.dropped_bytes == 3U);
    const auto resource_status = protocol::decode_resource_status(body(
        core.handle(request(protocol::Command::ResourceStatus,
                            protocol::encode_resource_id(kLossyResource)))));
    assert(resource_status.tx_buffered == 4U);
    assert(resource_status.tx_overruns == 3U);
    assert((resource_status.error_flags &
            protocol::kResourceErrorTxOverflow) != 0U);

    stream->set_failed(kLossyResource, true);
    assert(stream->produce(kLosslessResource, {42U}) ==
           mock_mcu::StreamPushStatus::Accepted);
    events = core.poll_stream_events();
    assert(events.size() == 1U);
    data = protocol::decode_stream_data(events.front().payload);
    assert(data.stream_id == first.stream_id && data.data[0] == 42U);
    assert(stream_status(core, lossy.stream_id).state ==
           protocol::StreamState::Failed);

    body(core.handle(request(protocol::Command::StreamStop,
                             protocol::encode_resource_id(first.stream_id))));
    const auto second = open(core, kLosslessResource, true);
    assert(second.stream_id != first.stream_id);
    assert(status(core.handle(request(
               protocol::Command::StreamCredit,
               protocol::encode_stream_credit(
                   {first.stream_id, 1U, 0U})))) ==
           mock_mcu::StatusCode::InvalidPayload);
}

void test_lease_timeout_clears_session_and_bytes() {
    auto stream = std::make_shared<mock_mcu::MockStreamBsp>();
    auto core = make_core(stream, false);
    const auto base = mock_mcu::RemoteCore::TimePoint{};
    assert(status(core.handle(request(
               protocol::Command::StreamOpen,
               protocol::encode_stream_open_request(
                   {kLosslessResource, 4U,
                    static_cast<std::uint16_t>(
                        protocol::kStreamFlagCreditRequired |
                        protocol::kStreamFlagLossless),
                    4U})), base)) == mock_mcu::StatusCode::AccessDenied);
    acquire(core, kLosslessResource, base, 100U);
    assert(status(core.handle(request(
               protocol::Command::StreamOpen,
               protocol::encode_stream_open_request(
                   {kLosslessResource, 4U,
                    static_cast<std::uint16_t>(
                        protocol::kStreamFlagCreditRequired |
                        protocol::kStreamFlagLossless),
                    0U})), base)) == mock_mcu::StatusCode::InvalidPayload);
    const auto opened = open(core, kLosslessResource, true, 4U, base);
    assert(stream->produce(kLosslessResource, {1U, 2U}) ==
           mock_mcu::StreamPushStatus::Accepted);
    auto prepared = core.prepare_stream_events(
        16U, base + std::chrono::milliseconds(99));
    assert(prepared.size() == 1U);
    assert(!core.commit_stream_event(
        prepared.front(), base + std::chrono::milliseconds(100)));
    assert(stream->buffered_bytes(kLosslessResource) == 2U);
    assert(core.expire_leases(base + std::chrono::milliseconds(100)) == 1U);
    assert(core.poll_stream_events().empty());
    const auto stopped = stream_status(core, opened.stream_id);
    assert(stopped.state == protocol::StreamState::Stopped);
    assert(stopped.buffered_bytes == 0U);
    assert(stopped.available_credit_bytes == 0U);
}

void test_outstanding_window_is_bounded() {
    constexpr std::uint32_t resource_id = 0x0F000013U;
    auto stream = std::make_shared<mock_mcu::MockStreamBsp>();
    stream->add_resource({
        resource_id, protocol::kStreamContractVersion,
        protocol::StreamDirection::NodeToHost,
        protocol::kStreamTransportUsb,
        protocol::kStreamFlagCreditRequired,
        1U, 128U, 100000U, 200000U, 1000U, 100U});
    mock_mcu::RemoteCore core(
        {}, mock_mcu::capability_mask(mock_mcu::Capability::Stream),
        nullptr, nullptr,
        {{resource_id, protocol::ResourceType::Stream, 3U,
          protocol::kResourceFlagNative, 0U, 128U}},
        {resource_contract(resource_id)}, nullptr, nullptr, nullptr,
        nullptr, nullptr, stream);
    acquire(core, resource_id);
    const auto opened = protocol::decode_stream_open_response(body(core.handle(
        request(protocol::Command::StreamOpen,
                protocol::encode_stream_open_request(
                    {resource_id, 1U, protocol::kStreamFlagCreditRequired,
                     128U})))));
    assert(stream->produce(resource_id, std::vector<std::uint8_t>(65U, 7U)) ==
           mock_mcu::StreamPushStatus::Accepted);
    for (const auto invalid_limit : {0U, 17U}) {
        bool rejected = false;
        try {
            static_cast<void>(core.poll_stream_events(invalid_limit));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
    }
    for (std::uint32_t sequence = 0U; sequence < 64U; ++sequence) {
        const auto events = core.poll_stream_events();
        assert(events.size() == 1U);
        assert(protocol::decode_stream_data(events.front().payload).sequence ==
               sequence);
    }
    assert(core.poll_stream_events().empty());
    auto current = stream_status(core, opened.stream_id);
    assert(current.state == protocol::StreamState::Backpressured);
    assert(current.buffered_bytes == 1U);
    assert(current.available_credit_bytes == 64U);
    body(core.handle(request(
        protocol::Command::StreamCredit,
        protocol::encode_stream_credit({opened.stream_id, 64U, 63U}))));
    const auto resumed = core.poll_stream_events();
    assert(resumed.size() == 1U);
    assert(protocol::decode_stream_data(resumed.front().payload).sequence ==
           64U);
}

void test_mock_node_fragments_n2h_event() {
    auto stream = std::make_shared<mock_mcu::MockStreamBsp>();
    auto core = make_core(stream, false);
    acquire(core, kLosslessResource);
    const auto opened = open(core, kLosslessResource, true, 8U);
    assert(stream->produce(kLosslessResource, {1U, 2U, 3U, 4U}) ==
           mock_mcu::StreamPushStatus::Accepted);
    mock_mcu::MockNode node(std::move(core), 32U, 7U);
    const auto replies = node.poll_stream_events();
    assert(replies.size() == 1U);
    assert(replies.front().frames.size() > 1U);
    protocol::Reassembler reassembler(32U, std::chrono::milliseconds(100));
    protocol::ReassemblyResult result;
    for (const auto& frame : replies.front().frames) {
        result = reassembler.accept(7U, frame);
    }
    assert(result.status == protocol::ReassemblyStatus::Complete);
    const auto packet = protocol::decode(*result.packet);
    assert(packet.header.command ==
           static_cast<std::uint16_t>(protocol::Command::StreamData));
    const auto data = protocol::decode_stream_data(packet.payload);
    assert(data.stream_id == opened.stream_id && data.sequence == 0U);

    // 每个节点持有独立 Core/BSP；另一节点后端失败不影响健康节点事件。
    auto failed_stream = std::make_shared<mock_mcu::MockStreamBsp>();
    auto failed_core = make_core(failed_stream, false);
    acquire(failed_core, kLosslessResource);
    static_cast<void>(open(failed_core, kLosslessResource, true, 4U));
    assert(failed_stream->produce(kLosslessResource, {9U}) ==
           mock_mcu::StreamPushStatus::Accepted);
    failed_stream->set_failed(kLosslessResource, true);
    mock_mcu::MockNode failed_node(std::move(failed_core), 32U, 8U);
    assert(failed_node.poll_stream_events().empty());
    assert(stream->produce(kLosslessResource, {5U}) ==
           mock_mcu::StreamPushStatus::Accepted);
    assert(node.poll_stream_events().size() == 1U);
}

void test_prepared_event_is_not_consumed_before_delivery_commit() {
    auto stream = std::make_shared<mock_mcu::MockStreamBsp>();
    auto core = make_core(stream, false);
    acquire(core, kLosslessResource);
    const auto opened = open(core, kLosslessResource, true, 4U);
    assert(stream->produce(kLosslessResource, {1U, 2U, 3U, 4U}) ==
           mock_mcu::StreamPushStatus::Accepted);

    auto prepared = core.prepare_stream_events();
    assert(prepared.size() == 1U);
    auto current = stream_status(core, opened.stream_id);
    assert(current.buffered_bytes == 4U);
    assert(current.available_credit_bytes == 4U);
    assert(current.next_sequence == 0U);

    // 模拟准备完成后、链路构造完成前后端失败。提交必须失败且不能删字节。
    stream->set_failed(kLosslessResource, true);
    assert(!core.commit_stream_event(prepared.front()));
    current = stream_status(core, opened.stream_id);
    assert(current.state == protocol::StreamState::Failed);
    assert(current.buffered_bytes == 4U);
    assert(current.available_credit_bytes == 4U);
    assert(current.next_sequence == 0U);
}

}  // namespace

int main() {
    test_credit_sequence_generation_and_overflow_isolation();
    test_lease_timeout_clears_session_and_bytes();
    test_outstanding_window_is_bounded();
    test_mock_node_fragments_n2h_event();
    test_prepared_event_is_not_consumed_before_delivery_commit();
    return 0;
}
