#include "remotebsp/toolbusd/ipc.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace remotebsp;

namespace {

toolbusd::MotionGroupPlan plan() {
    toolbusd::MotionGroupPlan result;
    result.transaction_id = 7001U;
    result.group_id = 91U;
    result.plan_generation = 5U;
    result.host_start_time_ns = 123456789000ULL;
    for (std::size_t index = 0U; index < result.content_digest.size();
         ++index) {
        result.content_digest[index] =
            static_cast<std::uint8_t>(index + 1U);
    }
    result.members = {
        {1U, {1U, result.host_start_time_ns, 1000000ULL, true,
              {{0x09000000U, 12}}}},
        {2U, {1U, result.host_start_time_ns, 2000000ULL, true,
              {{0x09000001U, -7}, {0x09000002U, 3}}}},
    };
    return result;
}

void check_plan(const toolbusd::MotionGroupPlan& value) {
    const auto expected = plan();
    assert(value.transaction_id == expected.transaction_id);
    assert(value.group_id == expected.group_id);
    assert(value.plan_generation == expected.plan_generation);
    assert(value.host_start_time_ns == expected.host_start_time_ns);
    assert(value.content_digest == expected.content_digest);
    assert(value.members.size() == expected.members.size());
    for (std::size_t index = 0U; index < value.members.size(); ++index) {
        assert(value.members[index].node_id ==
               expected.members[index].node_id);
        assert(protocol::encode_motion_segment(value.members[index].segment) ==
               protocol::encode_motion_segment(
                   expected.members[index].segment));
    }
}

void test_plan_and_snapshot_codec() {
    check_plan(toolbusd::decode_ipc_motion_group_plan(
        toolbusd::encode_ipc_motion_group_plan(plan())));

    toolbusd::MotionGroupServiceSnapshot snapshot;
    snapshot.transaction_id = 7001U;
    snapshot.group_id = 91U;
    snapshot.plan_generation = 5U;
    snapshot.state = toolbusd::MotionGroupState::Aborting;
    snapshot.abort_reason = protocol::MotionGroupAbortReason::CommitTimedOut;
    snapshot.member_count = 2U;
    snapshot.ready_count = 2U;
    snapshot.committed_count = 1U;
    snapshot.pending_request_count = 2U;
    snapshot.commit_dispatched = true;
    snapshot.abort_is_best_effort = true;
    const auto decoded = toolbusd::decode_ipc_motion_group_snapshot(
        toolbusd::encode_ipc_motion_group_snapshot(snapshot));
    assert(decoded.transaction_id == snapshot.transaction_id);
    assert(decoded.group_id == snapshot.group_id);
    assert(decoded.plan_generation == snapshot.plan_generation);
    assert(decoded.state == snapshot.state);
    assert(decoded.abort_reason == snapshot.abort_reason);
    assert(decoded.member_count == 2U);
    assert(decoded.ready_count == 2U);
    assert(decoded.committed_count == 1U);
    assert(decoded.pending_request_count == 2U);
    assert(decoded.commit_dispatched);
    assert(decoded.abort_is_best_effort);
}

template <typename Writer>
toolbusd::IpcRequest round_trip_request(Writer writer) {
    int sockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    writer(sockets[0]);
    const auto request = toolbusd::read_ipc_request(sockets[1]);
    ::close(sockets[0]);
    ::close(sockets[1]);
    return request;
}

void test_request_kinds_and_identity() {
    const auto submit = round_trip_request([](int socket) {
        toolbusd::write_ipc_motion_group_submit_request(socket, plan());
    });
    assert(submit.kind == toolbusd::IpcRequestKind::MotionGroupSubmit);
    check_plan(submit.motion_group_plan);

    const auto status = round_trip_request([](int socket) {
        toolbusd::write_ipc_motion_group_status_request(
            socket, 7001U, 91U, 5U);
    });
    assert(status.kind == toolbusd::IpcRequestKind::MotionGroupStatus);
    assert(status.transaction_id == 7001U);
    assert(status.group_id == 91U);
    assert(status.plan_generation == 5U);

    const auto cancel = round_trip_request([](int socket) {
        toolbusd::write_ipc_motion_group_cancel_request(
            socket, 7001U, 91U, 5U);
    });
    assert(cancel.kind == toolbusd::IpcRequestKind::MotionGroupCancel);
    assert(cancel.transaction_id == 7001U);
    assert(cancel.group_id == 91U);
    assert(cancel.plan_generation == 5U);
}

void test_codec_rejects_unbounded_or_inconsistent_values() {
    auto empty = plan();
    empty.members.clear();
    try {
        static_cast<void>(toolbusd::encode_ipc_motion_group_plan(empty));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }

    toolbusd::MotionGroupServiceSnapshot invalid;
    invalid.member_count = 1U;
    invalid.ready_count = 2U;
    try {
        static_cast<void>(
            toolbusd::encode_ipc_motion_group_snapshot(invalid));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }
}

}

int main() {
    test_plan_and_snapshot_codec();
    test_request_kinds_and_identity();
    test_codec_rejects_unbounded_or_inconsistent_values();
    return 0;
}
