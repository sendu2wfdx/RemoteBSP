#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RBSP_MOTION_QUEUE_LOW_WATERMARK 1U

#include "remotebsp_config.h"

#if defined(CONFIG_REMOTEBSP_MOTION)

#define RBSP_MOTION_PROTOCOL_MAX_AXES 64U
#define RBSP_MOTION_NO_DEADLINE UINT64_MAX

#if CONFIG_MOTION_MAX_AXES < 1 || \
    CONFIG_MOTION_MAX_AXES > RBSP_MOTION_PROTOCOL_MAX_AXES
#error "本板运动轴数超出协议范围"
#endif

#if CONFIG_MOTION_QUEUE_DEPTH < 2
#error "运动段队列深度至少为2"
#endif

#if CONFIG_MOTION_STEP_PULSE_WIDTH_US < 1 || \
    CONFIG_MOTION_MIN_STEP_LOW_US < 1
#error "STEP 高、低电平时间必须至少为1微秒"
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
    RBSP_MOTION_FAULT_TIMING = 4,
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
    /*
     * 单段步数来自 int32_t，绝对值最大为 2^31；下列逐段计数与
     * Bresenham 除数/余数因此用 uint32_t 即可完整表达。累计位置和
     * 单调时间仍保留 64 bit，避免以缩小数值范围换取 SRAM。
     */
    uint32_t active_emitted[CONFIG_MOTION_MAX_AXES];
    /*
     * 每个运动段启动时只做一次除法，将边沿间隔拆成整数商和余数。
     * compare ISR 中只需加法和比较，即可用 Bresenham/DDA 均匀分配余数。
     */
    uint32_t active_target[CONFIG_MOTION_MAX_AXES];
    uint64_t active_next_rise_ns[CONFIG_MOTION_MAX_AXES];
    uint64_t active_fall_ns[CONFIG_MOTION_MAX_AXES];
    uint64_t last_step_fall_ns[CONFIG_MOTION_MAX_AXES];
    uint64_t active_interval_ns[CONFIG_MOTION_MAX_AXES];
    uint32_t active_interval_remainder[CONFIG_MOTION_MAX_AXES];
    uint32_t active_interval_error[CONFIG_MOTION_MAX_AXES];
    uint32_t active_interval_divisor[CONFIG_MOTION_MAX_AXES];
    uint32_t maximum_step_rate_hz[CONFIG_MOTION_MAX_AXES];
    uint64_t next_deadline_ns;
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
    uint8_t axis_count;
    uint16_t queue_depth;
    uint16_t queue_low_watermark;
    bool queue_low;
    rbsp_motion_state_t state;
    rbsp_motion_fault_t fault;
    uint32_t last_accepted_sequence;
    uint32_t last_completed_sequence;
    uint64_t accepted_segments;
    uint64_t rejected_segments;
    uint64_t completed_segments;
    uint64_t emitted_edges;
    uint64_t safety_stops;
    uint64_t limit_stops;
    uint64_t queue_underruns;
    uint16_t maximum_queue_depth;
    uint64_t emitted_steps[CONFIG_MOTION_MAX_AXES];
    int64_t position_steps[CONFIG_MOTION_MAX_AXES];
    bool step_high[CONFIG_MOTION_MAX_AXES];
    bool enabled[CONFIG_MOTION_MAX_AXES];
    bool direction_positive[CONFIG_MOTION_MAX_AXES];
} rbsp_motion_status_snapshot_t;

typedef struct {
    bool (*set_enable)(uint8_t axis, bool enabled);
    bool (*set_direction)(uint8_t axis, bool positive);
    bool (*set_step)(uint8_t axis, bool high);
    bool (*limit_active)(uint8_t axis, bool* active);
} rbsp_motion_io_t;

bool rbsp_motion_init(rbsp_motion_queue_t* queue, uint8_t axis_count);
bool rbsp_motion_set_axis_rate_limit(rbsp_motion_queue_t* queue,
                                     uint8_t axis,
                                     uint32_t maximum_step_rate_hz);
bool rbsp_motion_validate_segment(
    const rbsp_motion_queue_t* queue,
    const rbsp_motion_segment_t* requested);
rbsp_motion_enqueue_result_t rbsp_motion_commit_segment(
    rbsp_motion_queue_t* queue,
    const rbsp_motion_segment_t* validated,
    uint64_t now_ns,
    rbsp_motion_segment_t* accepted);
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
bool rbsp_motion_snapshot(
    const rbsp_motion_queue_t* queue,
    rbsp_motion_status_snapshot_t* snapshot);
bool rbsp_motion_tick(rbsp_motion_queue_t* queue,
                      const rbsp_motion_io_t* io,
                      uint64_t now_ns);
bool rbsp_motion_service(rbsp_motion_queue_t* queue,
                         const rbsp_motion_io_t* io,
                         uint64_t now_ns,
                         uint64_t* next_deadline_ns);

/*
 * 更新一个逻辑轴的 EN 请求，并计算同一物理 EN 组的合并状态。
 * 只要组内任意轴仍请求使能，物理 EN 就保持有效；最后一个轴释放后才关闭。
 */
bool rbsp_motion_shared_enable_update(
    const uint16_t* group_ids, bool* axis_requests,
    uint8_t axis_count, uint8_t axis, bool enabled,
    bool* group_enabled);

#endif
