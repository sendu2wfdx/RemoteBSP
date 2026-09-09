#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "remotebsp_embedded/motion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RBSP_MOTION_GROUP_DIGEST_SIZE 32U

typedef enum {
    RBSP_MOTION_GROUP_IDLE = 0,
    RBSP_MOTION_GROUP_PREPARED = 1,
    RBSP_MOTION_GROUP_ARMED = 2,
    RBSP_MOTION_GROUP_ABORTED = 3,
    RBSP_MOTION_GROUP_COMPLETED = 4,
} rbsp_motion_group_state_t;

typedef enum {
    RBSP_MOTION_GROUP_READY = 0,
    RBSP_MOTION_GROUP_REJECTED = 1,
    RBSP_MOTION_GROUP_BUSY = 2,
    RBSP_MOTION_GROUP_CLOCK_MISMATCH = 3,
    RBSP_MOTION_GROUP_RESOURCE_UNAVAILABLE = 4,
} rbsp_motion_group_ready_code_t;

typedef enum {
    RBSP_MOTION_GROUP_COMMIT_ARMED = 0,
    RBSP_MOTION_GROUP_COMMIT_REJECTED = 1,
    RBSP_MOTION_GROUP_NOT_PREPARED = 2,
    RBSP_MOTION_GROUP_IDENTITY_MISMATCH = 3,
} rbsp_motion_group_commit_code_t;

typedef struct {
    uint64_t transaction_id;
    uint32_t group_id;
    uint32_t plan_generation;
    uint64_t boot_epoch;
    uint64_t clock_model_generation;
    uint64_t node_start_tick;
    uint8_t content_digest[RBSP_MOTION_GROUP_DIGEST_SIZE];
} rbsp_motion_group_identity_t;

typedef struct {
    rbsp_motion_group_state_t state;
    uint32_t owner_session_id;
    uint64_t boot_epoch;
    rbsp_motion_group_identity_t identity;
    rbsp_motion_segment_t segment;
} rbsp_motion_group_participant_t;

bool rbsp_motion_group_init(rbsp_motion_group_participant_t* participant,
                            uint64_t boot_epoch);
rbsp_motion_group_ready_code_t rbsp_motion_group_prepare(
    rbsp_motion_group_participant_t* participant,
    const rbsp_motion_queue_t* queue,
    const rbsp_motion_group_identity_t* identity,
    const rbsp_motion_segment_t* segment, uint32_t session_id,
    uint64_t now_ns);
rbsp_motion_group_commit_code_t rbsp_motion_group_commit(
    rbsp_motion_group_participant_t* participant,
    rbsp_motion_queue_t* queue,
    const rbsp_motion_group_identity_t* identity,
    uint32_t session_id, uint64_t now_ns,
    rbsp_motion_segment_t* accepted);
bool rbsp_motion_group_abort(
    rbsp_motion_group_participant_t* participant,
    const rbsp_motion_group_identity_t* identity,
    uint32_t session_id);
bool rbsp_motion_group_emergency_abort(
    rbsp_motion_group_participant_t* participant);
void rbsp_motion_group_observe_motion(
    rbsp_motion_group_participant_t* participant,
    const rbsp_motion_queue_t* queue);
bool rbsp_motion_group_blocks_enqueue(
    const rbsp_motion_group_participant_t* participant);

#ifdef __cplusplus
}
#endif
