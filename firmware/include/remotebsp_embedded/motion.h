#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "remotebsp_config.h"

#if defined(CONFIG_REMOTEBSP_MOTION)

#define RBSP_MOTION_PROTOCOL_MAX_AXES 64U

#if CONFIG_MOTION_MAX_AXES < 1 || \
    CONFIG_MOTION_MAX_AXES > RBSP_MOTION_PROTOCOL_MAX_AXES
#error "本板运动轴数超出协议范围"
#endif

#if CONFIG_MOTION_QUEUE_DEPTH < 2
#error "运动段队列深度至少为2"
#endif

typedef enum {
    RBSP_MOTION_IDLE = 0,
    RBSP_MOTION_ARMED = 1,
    RBSP_MOTION_RUNNING = 2,
    RBSP_MOTION_FAULTED = 3,
} rbsp_motion_state_t;

typedef enum {
    RBSP_MOTION_FAULT_NONE = 0,
    RBSP_MOTION_FAULT_ABORTED = 1,
    RBSP_MOTION_FAULT_LIMIT = 2,
    RBSP_MOTION_FAULT_UNDERRUN = 3,
} rbsp_motion_fault_t;

typedef enum {
    RBSP_MOTION_ENQUEUE_OK = 0,
    RBSP_MOTION_ENQUEUE_INVALID = 1,
    RBSP_MOTION_ENQUEUE_SEQUENCE = 2,
    RBSP_MOTION_ENQUEUE_LATE = 3,
    RBSP_MOTION_ENQUEUE_DISCONTINUOUS = 4,
    RBSP_MOTION_ENQUEUE_FULL = 5,
    RBSP_MOTION_ENQUEUE_FAULTED = 6,
} rbsp_motion_enqueue_result_t;

typedef struct {
    uint32_t sequence;
    uint64_t start_time_ns;
    uint64_t duration_ns;
    uint8_t axis_count;
    bool final_segment;
    int32_t steps[CONFIG_MOTION_MAX_AXES];
} rbsp_motion_segment_t;

typedef struct {
    rbsp_motion_segment_t segments[CONFIG_MOTION_QUEUE_DEPTH];
    uint8_t axis_count;
    uint16_t head;
    uint16_t size;
    uint32_t last_accepted_sequence;
    uint32_t last_completed_sequence;
    uint64_t next_available_time_ns;
    rbsp_motion_state_t state;
    rbsp_motion_fault_t fault;
    uint64_t active_emitted[CONFIG_MOTION_MAX_AXES];
    /*
     * 每轴的 DDA 相位以“步数 × 纳秒”为单位累计。这样定时中断中
     * 不需要执行 64 位除法，适合没有硬件除法器的 Cortex-M0。
     */
    uint64_t active_phase[CONFIG_MOTION_MAX_AXES];
    uint64_t active_last_tick_ns;
    uint64_t emitted_steps[CONFIG_MOTION_MAX_AXES];
    int64_t position_steps[CONFIG_MOTION_MAX_AXES];
    bool step_high[CONFIG_MOTION_MAX_AXES];
    bool enabled[CONFIG_MOTION_MAX_AXES];
    bool direction_positive[CONFIG_MOTION_MAX_AXES];
    uint64_t accepted_segments;
    uint64_t rejected_segments;
    uint64_t completed_segments;
    uint64_t emitted_edges;
    uint64_t safety_stops;
    uint64_t limit_stops;
    uint64_t queue_underruns;
    uint16_t maximum_queue_depth;
} rbsp_motion_queue_t;

typedef struct {
    bool (*set_enable)(uint8_t axis, bool enabled);
    bool (*set_direction)(uint8_t axis, bool positive);
    bool (*set_step)(uint8_t axis, bool high);
    bool (*limit_active)(uint8_t axis, bool* active);
} rbsp_motion_io_t;

bool rbsp_motion_init(rbsp_motion_queue_t* queue, uint8_t axis_count);
rbsp_motion_enqueue_result_t rbsp_motion_enqueue(
    rbsp_motion_queue_t* queue, const rbsp_motion_segment_t* requested,
    uint64_t now_ns, rbsp_motion_segment_t* accepted);
const rbsp_motion_segment_t* rbsp_motion_front(
    const rbsp_motion_queue_t* queue);
bool rbsp_motion_complete_front(rbsp_motion_queue_t* queue);
void rbsp_motion_abort(rbsp_motion_queue_t* queue,
                       rbsp_motion_fault_t fault);
bool rbsp_motion_clear_fault(rbsp_motion_queue_t* queue,
                             uint64_t now_ns);
bool rbsp_motion_tick(rbsp_motion_queue_t* queue,
                      const rbsp_motion_io_t* io,
                      uint64_t now_ns);

#endif
