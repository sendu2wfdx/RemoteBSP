#include "remotebsp_embedded/motion_group.h"

#include <limits.h>
#include <string.h>

static bool digest_nonzero(const uint8_t* digest) {
    uint8_t combined = 0U;
    for (uint8_t index = 0U; index < RBSP_MOTION_GROUP_DIGEST_SIZE; ++index) {
        combined |= digest[index];
    }
    return combined != 0U;
}

static bool identity_valid(const rbsp_motion_group_identity_t* identity) {
    return identity != NULL && identity->transaction_id != 0U &&
           identity->group_id != 0U && identity->plan_generation != 0U &&
           identity->boot_epoch != 0U &&
           identity->clock_model_generation != 0U &&
           identity->node_start_tick != 0U &&
           digest_nonzero(identity->content_digest);
}

static bool same_identity(const rbsp_motion_group_identity_t* left,
                          const rbsp_motion_group_identity_t* right) {
    return left->transaction_id == right->transaction_id &&
           left->group_id == right->group_id &&
           left->plan_generation == right->plan_generation &&
           left->boot_epoch == right->boot_epoch &&
           left->clock_model_generation == right->clock_model_generation &&
           left->node_start_tick == right->node_start_tick &&
           memcmp(left->content_digest, right->content_digest,
                  RBSP_MOTION_GROUP_DIGEST_SIZE) == 0;
}

static bool same_segment(const rbsp_motion_segment_t* left,
                         const rbsp_motion_segment_t* right) {
    return left->sequence == right->sequence &&
           left->start_time_ns == right->start_time_ns &&
           left->duration_ns == right->duration_ns &&
           left->final_segment == right->final_segment &&
           left->axis_count == right->axis_count &&
           memcmp(left->steps, right->steps,
                  sizeof(left->steps[0]) * left->axis_count) == 0;
}

bool rbsp_motion_group_init(rbsp_motion_group_participant_t* participant,
                            uint64_t boot_epoch) {
    if (participant == NULL) {
        return false;
    }
    memset(participant, 0, sizeof(*participant));
    participant->boot_epoch = boot_epoch;
    return true;
}

rbsp_motion_group_ready_code_t rbsp_motion_group_prepare(
    rbsp_motion_group_participant_t* participant,
    const rbsp_motion_queue_t* queue,
    const rbsp_motion_group_identity_t* identity,
    const rbsp_motion_segment_t* segment, uint32_t session_id,
    uint64_t now_ns) {
    if (participant == NULL || queue == NULL || segment == NULL ||
        session_id == 0U || !identity_valid(identity) ||
        segment->axis_count != queue->axis_count ||
        segment->axis_count == 0U ||
        segment->axis_count > CONFIG_MOTION_MAX_AXES) {
        return RBSP_MOTION_GROUP_REJECTED;
    }
    if (identity->boot_epoch != participant->boot_epoch) {
        return RBSP_MOTION_GROUP_CLOCK_MISMATCH;
    }
    rbsp_motion_group_observe_motion(participant, queue);
    if (participant->state == RBSP_MOTION_GROUP_ABORTED ||
        participant->state == RBSP_MOTION_GROUP_COMPLETED) {
        const bool stale = identity->group_id == participant->identity.group_id &&
                           identity->plan_generation <=
                               participant->identity.plan_generation;
        if (stale) {
            return RBSP_MOTION_GROUP_BUSY;
        }
        participant->state = RBSP_MOTION_GROUP_IDLE;
        participant->owner_session_id = 0U;
    }
    if (participant->state != RBSP_MOTION_GROUP_IDLE) {
        return participant->owner_session_id == session_id &&
                       same_identity(&participant->identity, identity) &&
                       same_segment(&participant->segment, segment)
                   ? RBSP_MOTION_GROUP_READY
                   : RBSP_MOTION_GROUP_BUSY;
    }
    if (queue->state != RBSP_MOTION_IDLE || queue->size != 0U ||
        queue->fault != RBSP_MOTION_FAULT_NONE) {
        return RBSP_MOTION_GROUP_BUSY;
    }
    if (UINT64_MAX - now_ns <
            (uint64_t)CONFIG_MOTION_MIN_LEAD_TIME_US * 1000U ||
        identity->node_start_tick <
            now_ns + (uint64_t)CONFIG_MOTION_MIN_LEAD_TIME_US * 1000U) {
        return RBSP_MOTION_GROUP_CLOCK_MISMATCH;
    }
    rbsp_motion_segment_t local = *segment;
    local.start_time_ns = identity->node_start_tick;
    rbsp_motion_queue_t probe = *queue;
    if (!rbsp_motion_validate_segment(queue, &local) ||
        rbsp_motion_commit_segment(&probe, &local, now_ns, NULL) !=
            RBSP_MOTION_ENQUEUE_OK) {
        return RBSP_MOTION_GROUP_REJECTED;
    }
    participant->identity = *identity;
    participant->segment = local;
    participant->owner_session_id = session_id;
    participant->state = RBSP_MOTION_GROUP_PREPARED;
    return RBSP_MOTION_GROUP_READY;
}

rbsp_motion_group_commit_code_t rbsp_motion_group_commit(
    rbsp_motion_group_participant_t* participant,
    rbsp_motion_queue_t* queue,
    const rbsp_motion_group_identity_t* identity,
    uint32_t session_id, uint64_t now_ns,
    rbsp_motion_segment_t* accepted) {
    if (participant == NULL || queue == NULL || !identity_valid(identity)) {
        return RBSP_MOTION_GROUP_COMMIT_REJECTED;
    }
    if (participant->owner_session_id != session_id ||
        !same_identity(&participant->identity, identity)) {
        return participant->state == RBSP_MOTION_GROUP_IDLE
                   ? RBSP_MOTION_GROUP_NOT_PREPARED
                   : RBSP_MOTION_GROUP_IDENTITY_MISMATCH;
    }
    if (participant->state == RBSP_MOTION_GROUP_ARMED ||
        participant->state == RBSP_MOTION_GROUP_COMPLETED) {
        return RBSP_MOTION_GROUP_COMMIT_ARMED;
    }
    if (participant->state != RBSP_MOTION_GROUP_PREPARED) {
        return RBSP_MOTION_GROUP_NOT_PREPARED;
    }
    const rbsp_motion_enqueue_result_t result = rbsp_motion_commit_segment(
        queue, &participant->segment, now_ns, accepted);
    if (result != RBSP_MOTION_ENQUEUE_OK) {
        return RBSP_MOTION_GROUP_COMMIT_REJECTED;
    }
    participant->state = RBSP_MOTION_GROUP_ARMED;
    return RBSP_MOTION_GROUP_COMMIT_ARMED;
}

bool rbsp_motion_group_abort(
    rbsp_motion_group_participant_t* participant,
    const rbsp_motion_group_identity_t* identity,
    uint32_t session_id) {
    if (participant == NULL || identity == NULL ||
        participant->state == RBSP_MOTION_GROUP_IDLE ||
        participant->owner_session_id != session_id ||
        !same_identity(&participant->identity, identity)) {
        return false;
    }
    participant->state = RBSP_MOTION_GROUP_ABORTED;
    return true;
}

bool rbsp_motion_group_emergency_abort(
    rbsp_motion_group_participant_t* participant) {
    if (participant == NULL ||
        participant->state == RBSP_MOTION_GROUP_IDLE) {
        return false;
    }
    participant->state = RBSP_MOTION_GROUP_ABORTED;
    return true;
}

void rbsp_motion_group_observe_motion(
    rbsp_motion_group_participant_t* participant,
    const rbsp_motion_queue_t* queue) {
    if (participant != NULL && queue != NULL &&
        participant->state == RBSP_MOTION_GROUP_ARMED &&
        queue->state == RBSP_MOTION_IDLE && queue->size == 0U &&
        queue->fault == RBSP_MOTION_FAULT_NONE) {
        participant->state = RBSP_MOTION_GROUP_COMPLETED;
    }
}

bool rbsp_motion_group_blocks_enqueue(
    const rbsp_motion_group_participant_t* participant) {
    return participant != NULL &&
           (participant->state == RBSP_MOTION_GROUP_PREPARED ||
            participant->state == RBSP_MOTION_GROUP_ARMED);
}
