#include "remotebsp_embedded/motion_group.h"

#include <assert.h>
#include <string.h>

static rbsp_motion_group_identity_t identity(uint32_t generation) {
    rbsp_motion_group_identity_t value;
    memset(&value, 0, sizeof(value));
    value.transaction_id = 17U;
    value.group_id = 23U;
    value.plan_generation = generation;
    value.boot_epoch = 31U;
    value.clock_model_generation = 41U;
    value.node_start_tick = 10000000U;
    value.content_digest[0] = 1U;
    return value;
}

static rbsp_motion_segment_t segment(void) {
    rbsp_motion_segment_t value;
    memset(&value, 0, sizeof(value));
    value.sequence = 1U;
    value.start_time_ns = 10000000U;
    value.duration_ns = 2000000U;
    value.final_segment = true;
    value.axis_count = 2U;
    value.steps[0] = 10;
    value.steps[1] = -5;
    return value;
}

int main(void) {
    rbsp_motion_queue_t queue;
    rbsp_motion_group_participant_t participant;
    assert(rbsp_motion_init(&queue, 2U));
    assert(rbsp_motion_group_init(&participant, 31U));

    rbsp_motion_group_identity_t frozen = identity(1U);
    rbsp_motion_segment_t prepared = segment();
    rbsp_motion_group_identity_t wrong_boot = frozen;
    ++wrong_boot.boot_epoch;
    assert(rbsp_motion_group_prepare(
               &participant, &queue, &wrong_boot, &prepared,
               UINT64_C(3), 0x1234U, 1000000U) ==
           RBSP_MOTION_GROUP_CLOCK_MISMATCH);
    assert(rbsp_motion_group_prepare(
               &participant, &queue, &frozen, &prepared,
               UINT64_C(3), 0x1234U, 1000000U) == RBSP_MOTION_GROUP_READY);
    assert(participant.state == RBSP_MOTION_GROUP_PREPARED);
    assert(rbsp_motion_group_owned_by(&participant, 0x1234U));
    assert(rbsp_motion_group_uses_axis(&participant, 0U));
    assert(rbsp_motion_group_uses_axis(&participant, 1U));
    assert(!rbsp_motion_group_uses_axis(&participant, 2U));
    assert(queue.size == 0U);
    assert(queue.accepted_segments == 0U);
    assert(rbsp_motion_group_blocks_enqueue(&participant));

    /* 不同 request_id 形成的重复 PREPARE/COMMIT 仍必须幂等。 */
    assert(rbsp_motion_group_prepare(
               &participant, &queue, &frozen, &prepared,
               UINT64_C(3), 0x1234U, 1000000U) == RBSP_MOTION_GROUP_READY);
    assert(rbsp_motion_group_prepare(
               &participant, &queue, &frozen, &prepared,
               UINT64_C(3), 0x9999U, 1000000U) == RBSP_MOTION_GROUP_BUSY);
    rbsp_motion_segment_t conflict = prepared;
    ++conflict.steps[0];
    assert(rbsp_motion_group_prepare(
               &participant, &queue, &frozen, &conflict,
               UINT64_C(3), 0x1234U, 1000000U) == RBSP_MOTION_GROUP_BUSY);
    assert(rbsp_motion_group_prepare(
               &participant, &queue, &frozen, &prepared,
               UINT64_C(1), 0x1234U, 1000000U) ==
           RBSP_MOTION_GROUP_BUSY);

    rbsp_motion_group_identity_t wrong = frozen;
    ++wrong.clock_model_generation;
    assert(rbsp_motion_group_commit(
               &participant, &queue, &wrong, 0x1234U,
               2000000U, NULL) ==
           RBSP_MOTION_GROUP_IDENTITY_MISMATCH);
    assert(queue.size == 0U);
    assert(rbsp_motion_group_commit(
               &participant, &queue, &frozen, 0x1234U,
               2000000U, NULL) == RBSP_MOTION_GROUP_COMMIT_ARMED);
    assert(participant.state == RBSP_MOTION_GROUP_ARMED);
    assert(queue.size == 1U);
    assert(queue.accepted_segments == 1U);
    assert(rbsp_motion_group_commit(
               &participant, &queue, &frozen, 0x1234U,
               2000000U, NULL) == RBSP_MOTION_GROUP_COMMIT_ARMED);
    assert(queue.size == 1U);
    assert(queue.accepted_segments == 1U);

    assert(!rbsp_motion_group_abort(
        &participant, &frozen, 0x9999U));
    assert(rbsp_motion_group_abort(
        &participant, &frozen, 0x1234U));
    assert(participant.state == RBSP_MOTION_GROUP_ABORTED);
    assert(!rbsp_motion_group_blocks_enqueue(&participant));

    /* 已 ABORT 的同组旧代次不能复活；更高代次可重新预备。 */
    assert(rbsp_motion_group_prepare(
               &participant, &queue, &frozen, &prepared,
               UINT64_C(3), 0x1234U, 2000000U) == RBSP_MOTION_GROUP_BUSY);
    rbsp_motion_abort(&queue, RBSP_MOTION_FAULT_ABORTED);
    assert(rbsp_motion_clear_fault(&queue, 2000000U));
    rbsp_motion_group_identity_t next = identity(2U);
    next.transaction_id = 18U;
    next.node_start_tick = 20000000U;
    prepared.sequence = 2U;
    prepared.start_time_ns = next.node_start_tick;
    assert(rbsp_motion_group_prepare(
               &participant, &queue, &next, &prepared,
               UINT64_C(3), 0x1234U, 3000000U) == RBSP_MOTION_GROUP_READY);
    assert(rbsp_motion_group_emergency_abort(&participant));
    assert(participant.state == RBSP_MOTION_GROUP_ABORTED);

    rbsp_motion_queue_t completed_queue;
    rbsp_motion_group_participant_t completed;
    assert(rbsp_motion_init(&completed_queue, 2U));
    assert(rbsp_motion_group_init(&completed, 31U));
    frozen = identity(1U);
    prepared = segment();
    assert(rbsp_motion_group_prepare(
               &completed, &completed_queue, &frozen, &prepared,
               UINT64_C(3), 0x1234U, 1000000U) == RBSP_MOTION_GROUP_READY);
    assert(rbsp_motion_group_commit(
               &completed, &completed_queue, &frozen, 0x1234U,
               2000000U, NULL) == RBSP_MOTION_GROUP_COMMIT_ARMED);
    assert(rbsp_motion_complete_front(&completed_queue));
    rbsp_motion_group_observe_motion(&completed, &completed_queue);
    assert(completed.state == RBSP_MOTION_GROUP_COMPLETED);
    assert(!rbsp_motion_group_blocks_enqueue(&completed));
    assert(rbsp_motion_group_commit(
               &completed, &completed_queue, &frozen, 0x1234U,
               3000000U, NULL) == RBSP_MOTION_GROUP_COMMIT_ARMED);
    assert(completed_queue.size == 0U);
    assert(rbsp_motion_group_prepare(
               &completed, &completed_queue, &frozen, &prepared,
               UINT64_C(3), 0x1234U, 3000000U) == RBSP_MOTION_GROUP_BUSY);
    assert(completed_queue.size == 0U &&
           completed_queue.accepted_segments == 1U);
    rbsp_motion_segment_t ordinary = prepared;
    ordinary.sequence = 2U;
    ordinary.start_time_ns = 20000000U;
    assert(rbsp_motion_enqueue(
               &completed_queue, &ordinary, 3000000U, NULL) ==
           RBSP_MOTION_ENQUEUE_OK);
    assert(completed_queue.size == 1U);
    rbsp_motion_abort(&completed_queue, RBSP_MOTION_FAULT_ABORTED);
    assert(rbsp_motion_clear_fault(&completed_queue, 3000000U));
    next = identity(2U);
    next.transaction_id = 18U;
    next.node_start_tick = 30000000U;
    prepared.sequence = 3U;
    prepared.start_time_ns = next.node_start_tick;
    assert(rbsp_motion_group_prepare(
               &completed, &completed_queue, &next, &prepared,
               UINT64_C(3), 0x1234U, 3000000U) == RBSP_MOTION_GROUP_READY);
    return 0;
}
