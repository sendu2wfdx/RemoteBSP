#include "remotebsp_embedded/motion.h"

#if defined(CONFIG_REMOTEBSP_MOTION)

#include <limits.h>
#include <stddef.h>
#include <string.h>

bool rbsp_motion_shared_enable_update(
    const uint16_t* group_ids, bool* axis_requests,
    uint8_t axis_count, uint8_t axis, bool enabled,
    bool* group_enabled) {
    if (group_ids == NULL || axis_requests == NULL || group_enabled == NULL ||
        axis_count == 0U || axis >= axis_count) {
        return false;
    }
    axis_requests[axis] = enabled;
    *group_enabled = false;
    const uint16_t group = group_ids[axis];
    for (uint8_t index = 0U; index < axis_count; ++index) {
        if (group_ids[index] == group && axis_requests[index]) {
            *group_enabled = true;
            break;
        }
    }
    return true;
}

static uint16_t tail_index(const rbsp_motion_queue_t* queue) {
    return (uint16_t)(
        (queue->head + queue->size) % CONFIG_MOTION_QUEUE_DEPTH);
}

static uint64_t minimum_start_time(uint64_t now_ns) {
    const uint64_t lead_ns =
        (uint64_t)CONFIG_MOTION_MIN_LEAD_TIME_US * 1000ULL;
    if (UINT64_MAX - now_ns < lead_ns) {
        return UINT64_MAX;
    }
    return now_ns + lead_ns;
}

static uint64_t absolute_steps(int32_t steps) {
    return steps < 0
               ? (uint64_t)(-(int64_t)steps)
               : (uint64_t)steps;
}

static uint64_t maximum_events_for_duration(uint64_t duration_ns,
                                            uint32_t rate_hz) {
    const uint64_t whole_seconds = duration_ns / 1000000000ULL;
    const uint64_t remainder_ns = duration_ns % 1000000000ULL;
    if (whole_seconds > UINT64_MAX / rate_hz) {
        return UINT64_MAX;
    }
    const uint64_t whole = whole_seconds * rate_hz;
    const uint64_t partial =
        (remainder_ns * rate_hz) / 1000000000ULL;
    return UINT64_MAX - whole < partial
               ? UINT64_MAX
               : whole + partial;
}

static bool step_timing_valid(uint64_t duration_ns,
                              uint64_t steps) {
    if (steps == 0U) {
        return true;
    }
    const uint64_t pulse_ns =
        (uint64_t)CONFIG_MOTION_STEP_PULSE_WIDTH_US * 1000ULL;
    const uint64_t setup_ns =
        (uint64_t)CONFIG_MOTION_DIRECTION_SETUP_US * 1000ULL;
    const uint64_t low_ns =
        (uint64_t)CONFIG_MOTION_MIN_STEP_LOW_US * 1000ULL;
    const uint64_t first_rise_delay_ns =
        setup_ns > low_ns ? setup_ns : low_ns;
    if (duration_ns < first_rise_delay_ns + pulse_ns) {
        return false;
    }
    if (steps == 1U) {
        return true;
    }
    const uint64_t span_ns =
        duration_ns - first_rise_delay_ns - pulse_ns;
    return span_ns / (steps - 1U) >= pulse_ns + low_ns;
}

bool rbsp_motion_init(rbsp_motion_queue_t* queue, uint8_t axis_count) {
    if (queue == NULL || axis_count == 0U ||
        axis_count > CONFIG_MOTION_MAX_AXES) {
        return false;
    }
    memset(queue, 0, sizeof(*queue));
    queue->axis_count = axis_count;
    queue->state = RBSP_MOTION_IDLE;
    queue->next_deadline_ns = RBSP_MOTION_NO_DEADLINE;
    for (uint8_t axis = 0U; axis < axis_count; ++axis) {
        queue->maximum_step_rate_hz[axis] =
            CONFIG_MOTION_MAX_STEP_RATE_HZ;
    }
    return true;
}

bool rbsp_motion_set_axis_rate_limit(rbsp_motion_queue_t* queue,
                                     uint8_t axis,
                                     uint32_t maximum_step_rate_hz) {
    if (queue == NULL || axis >= queue->axis_count ||
        maximum_step_rate_hz == 0U ||
        maximum_step_rate_hz > CONFIG_MOTION_MAX_STEP_RATE_HZ ||
        queue->state != RBSP_MOTION_IDLE || queue->size != 0U) {
        return false;
    }
    queue->maximum_step_rate_hz[axis] = maximum_step_rate_hz;
    return true;
}

bool rbsp_motion_validate_segment(
    const rbsp_motion_queue_t* queue,
    const rbsp_motion_segment_t* requested) {
    if (queue == NULL || requested == NULL ||
        requested->sequence == 0U || requested->duration_ns == 0U ||
        requested->axis_count != queue->axis_count ||
        UINT64_MAX - requested->start_time_ns <
            requested->duration_ns) {
        return false;
    }
    const uint64_t maximum_total_steps = maximum_events_for_duration(
        requested->duration_ns, CONFIG_MOTION_MAX_TOTAL_STEP_RATE_HZ);
    uint64_t total_steps = 0U;
    for (uint8_t axis = 0U;
         axis < queue->axis_count; ++axis) {
        const uint64_t steps =
            absolute_steps(requested->steps[axis]);
        const uint64_t maximum_steps = maximum_events_for_duration(
            requested->duration_ns,
            queue->maximum_step_rate_hz[axis]);
        if (steps > maximum_steps || !step_timing_valid(
                requested->duration_ns, steps) ||
            (steps != 0U &&
             requested->duration_ns > UINT64_MAX / steps)) {
            return false;
        }
        if (UINT64_MAX - total_steps < steps) {
            return false;
        }
        total_steps += steps;
    }
    if (total_steps > maximum_total_steps) {
        return false;
    }
    return true;
}

rbsp_motion_enqueue_result_t rbsp_motion_commit_segment(
    rbsp_motion_queue_t* queue,
    const rbsp_motion_segment_t* requested,
    uint64_t now_ns,
    rbsp_motion_segment_t* accepted) {
    if (queue == NULL || requested == NULL) {
        return RBSP_MOTION_ENQUEUE_INVALID;
    }
    if (queue->fault != RBSP_MOTION_FAULT_NONE) {
        ++queue->rejected_segments;
        return RBSP_MOTION_ENQUEUE_FAULTED;
    }
    if (queue->size >= CONFIG_MOTION_QUEUE_DEPTH) {
        ++queue->rejected_segments;
        return RBSP_MOTION_ENQUEUE_FULL;
    }
    if (requested->sequence !=
        queue->last_accepted_sequence + 1U) {
        return RBSP_MOTION_ENQUEUE_SEQUENCE;
    }
    if (queue->size > 0U) {
        const uint16_t last_index = (uint16_t)(
            (queue->head + queue->size - 1U) %
            CONFIG_MOTION_QUEUE_DEPTH);
        if (queue->segments[last_index].final_segment) {
            return RBSP_MOTION_ENQUEUE_INVALID;
        }
    }

    const bool queue_was_empty = queue->size == 0U;
    rbsp_motion_segment_t value = *requested;
    const uint64_t earliest = minimum_start_time(now_ns);
    if (value.start_time_ns == 0U) {
        value.start_time_ns =
            queue->next_available_time_ns > earliest
                ? queue->next_available_time_ns
                : earliest;
    } else if (value.start_time_ns < earliest) {
        return RBSP_MOTION_ENQUEUE_LATE;
    }
    if (queue->size > 0U &&
        value.start_time_ns != queue->next_available_time_ns) {
        return RBSP_MOTION_ENQUEUE_DISCONTINUOUS;
    }
    if (UINT64_MAX - value.start_time_ns < value.duration_ns) {
        return RBSP_MOTION_ENQUEUE_INVALID;
    }

    queue->segments[tail_index(queue)] = value;
    ++queue->size;
    queue->last_accepted_sequence = value.sequence;
    queue->next_available_time_ns =
        value.start_time_ns + value.duration_ns;
    if (queue_was_empty) {
        queue->state = RBSP_MOTION_ARMED;
        queue->next_deadline_ns = value.start_time_ns;
    }
    ++queue->accepted_segments;
    if (queue->size > queue->maximum_queue_depth) {
        queue->maximum_queue_depth = queue->size;
    }
    if (accepted != NULL) {
        *accepted = value;
    }
    return RBSP_MOTION_ENQUEUE_OK;
}

rbsp_motion_enqueue_result_t rbsp_motion_enqueue(
    rbsp_motion_queue_t* queue, const rbsp_motion_segment_t* requested,
    uint64_t now_ns, rbsp_motion_segment_t* accepted) {
    if (!rbsp_motion_validate_segment(queue, requested)) {
        if (queue != NULL) {
            ++queue->rejected_segments;
        }
        return RBSP_MOTION_ENQUEUE_INVALID;
    }
    return rbsp_motion_commit_segment(
        queue, requested, now_ns, accepted);
}

const rbsp_motion_segment_t* rbsp_motion_front(
    const rbsp_motion_queue_t* queue) {
    if (queue == NULL || queue->size == 0U) {
        return NULL;
    }
    return &queue->segments[queue->head];
}

bool rbsp_motion_complete_front(rbsp_motion_queue_t* queue) {
    if (queue == NULL || queue->size == 0U ||
        queue->fault != RBSP_MOTION_FAULT_NONE) {
        return false;
    }
    const bool final_segment =
        queue->segments[queue->head].final_segment;
    queue->last_completed_sequence =
        queue->segments[queue->head].sequence;
    memset(&queue->segments[queue->head], 0,
           sizeof(queue->segments[queue->head]));
    queue->head = (uint16_t)(
        (queue->head + 1U) % CONFIG_MOTION_QUEUE_DEPTH);
    --queue->size;
    if (final_segment) {
        queue->size = 0U;
        queue->state = RBSP_MOTION_IDLE;
        queue->next_deadline_ns = RBSP_MOTION_NO_DEADLINE;
    } else if (queue->size == 0U) {
        queue->state = RBSP_MOTION_FAULTED;
        queue->fault = RBSP_MOTION_FAULT_UNDERRUN;
        ++queue->queue_underruns;
        ++queue->safety_stops;
    } else {
        queue->state = RBSP_MOTION_ARMED;
    }
    ++queue->completed_segments;
    return true;
}

void rbsp_motion_abort(rbsp_motion_queue_t* queue,
                       rbsp_motion_fault_t fault) {
    if (queue == NULL || fault == RBSP_MOTION_FAULT_NONE) {
        return;
    }
    memset(queue->segments, 0, sizeof(queue->segments));
    queue->head = 0U;
    queue->size = 0U;
    queue->state = RBSP_MOTION_FAULTED;
    queue->fault = fault;
    queue->next_deadline_ns = RBSP_MOTION_NO_DEADLINE;
    ++queue->safety_stops;
    if (fault == RBSP_MOTION_FAULT_LIMIT) {
        ++queue->limit_stops;
    }
}

bool rbsp_motion_clear_fault(rbsp_motion_queue_t* queue,
                             uint64_t now_ns) {
    if (queue == NULL || queue->size != 0U) {
        return false;
    }
    queue->fault = RBSP_MOTION_FAULT_NONE;
    queue->state = RBSP_MOTION_IDLE;
    queue->next_available_time_ns = now_ns;
    queue->next_deadline_ns = RBSP_MOTION_NO_DEADLINE;
    return true;
}

bool rbsp_motion_snapshot(
    const rbsp_motion_queue_t* queue,
    rbsp_motion_status_snapshot_t* snapshot) {
    if (queue == NULL || snapshot == NULL) {
        return false;
    }
    snapshot->axis_count = queue->axis_count;
    snapshot->queue_depth = queue->size;
    snapshot->state = queue->state;
    snapshot->fault = queue->fault;
    snapshot->last_accepted_sequence =
        queue->last_accepted_sequence;
    snapshot->last_completed_sequence =
        queue->last_completed_sequence;
    snapshot->accepted_segments = queue->accepted_segments;
    snapshot->rejected_segments = queue->rejected_segments;
    snapshot->completed_segments = queue->completed_segments;
    snapshot->emitted_edges = queue->emitted_edges;
    snapshot->safety_stops = queue->safety_stops;
    snapshot->limit_stops = queue->limit_stops;
    snapshot->queue_underruns = queue->queue_underruns;
    snapshot->maximum_queue_depth = queue->maximum_queue_depth;
    for (uint8_t axis = 0U; axis < queue->axis_count; ++axis) {
        snapshot->emitted_steps[axis] =
            queue->emitted_steps[axis];
        snapshot->position_steps[axis] =
            queue->position_steps[axis];
        snapshot->step_high[axis] = queue->step_high[axis];
        snapshot->enabled[axis] = queue->enabled[axis];
        snapshot->direction_positive[axis] =
            queue->direction_positive[axis];
    }
    return true;
}

static bool set_segment_enabled(rbsp_motion_queue_t* queue,
                                const rbsp_motion_io_t* io,
                                const rbsp_motion_segment_t* segment) {
    for (uint8_t axis = 0U;
         axis < queue->axis_count; ++axis) {
        const bool enabled = segment->steps[axis] != 0;
        if (queue->enabled[axis] == enabled) {
            continue;
        }
        if (!io->set_enable(axis, enabled)) {
            return false;
        }
        queue->enabled[axis] = enabled;
    }
    return true;
}

static void stop_outputs(rbsp_motion_queue_t* queue,
                         const rbsp_motion_io_t* io) {
    for (uint8_t axis = 0U;
         axis < queue->axis_count; ++axis) {
        if (queue->step_high[axis]) {
            (void)io->set_step(axis, false);
            queue->step_high[axis] = false;
            ++queue->emitted_edges;
        }
        if (queue->enabled[axis]) {
            (void)io->set_enable(axis, false);
            queue->enabled[axis] = false;
        }
    }
}

static bool start_segment(rbsp_motion_queue_t* queue,
                          const rbsp_motion_io_t* io,
                          const rbsp_motion_segment_t* segment) {
    memset(queue->active_emitted, 0,
           sizeof(queue->active_emitted));
    memset(queue->active_target, 0,
           sizeof(queue->active_target));
    memset(queue->active_next_rise_ns, 0,
           sizeof(queue->active_next_rise_ns));
    memset(queue->active_fall_ns, 0,
           sizeof(queue->active_fall_ns));
    memset(queue->active_interval_ns, 0,
           sizeof(queue->active_interval_ns));
    memset(queue->active_interval_remainder, 0,
           sizeof(queue->active_interval_remainder));
    memset(queue->active_interval_error, 0,
           sizeof(queue->active_interval_error));
    memset(queue->active_interval_divisor, 0,
           sizeof(queue->active_interval_divisor));

    const uint64_t setup_ns =
        (uint64_t)CONFIG_MOTION_DIRECTION_SETUP_US * 1000ULL;
    const uint64_t pulse_ns =
        (uint64_t)CONFIG_MOTION_STEP_PULSE_WIDTH_US * 1000ULL;
    const uint64_t low_ns =
        (uint64_t)CONFIG_MOTION_MIN_STEP_LOW_US * 1000ULL;
    const uint64_t first_rise_delay_ns =
        setup_ns > low_ns ? setup_ns : low_ns;
    for (uint8_t axis = 0U; axis < queue->axis_count; ++axis) {
        const bool positive = segment->steps[axis] >= 0;
        if (!io->set_direction(axis, positive)) {
            return false;
        }
        queue->direction_positive[axis] = positive;
        const uint64_t target = absolute_steps(segment->steps[axis]);
        queue->active_target[axis] = target;
        if (target == 0U) {
            continue;
        }
        queue->active_next_rise_ns[axis] =
            segment->start_time_ns + first_rise_delay_ns;
        if (target > 1U) {
            const uint64_t divisor = target - 1U;
            const uint64_t span_ns =
                segment->duration_ns - first_rise_delay_ns - pulse_ns;
            queue->active_interval_divisor[axis] = divisor;
            queue->active_interval_ns[axis] = span_ns / divisor;
            queue->active_interval_remainder[axis] =
                span_ns % divisor;
        }
    }
    if (!set_segment_enabled(queue, io, segment)) {
        return false;
    }
    queue->state = RBSP_MOTION_RUNNING;
    return true;
}

static uint64_t earliest_deadline(
    const rbsp_motion_queue_t* queue,
    const rbsp_motion_segment_t* segment) {
    if (queue->state == RBSP_MOTION_ARMED) {
        return segment->start_time_ns;
    }
    uint64_t deadline =
        segment->start_time_ns + segment->duration_ns;
    for (uint8_t axis = 0U; axis < queue->axis_count; ++axis) {
        if (queue->step_high[axis] &&
            queue->active_fall_ns[axis] < deadline) {
            deadline = queue->active_fall_ns[axis];
        }
        if (queue->active_emitted[axis] <
                queue->active_target[axis] &&
            queue->active_next_rise_ns[axis] < deadline) {
            deadline = queue->active_next_rise_ns[axis];
        }
    }
    return deadline;
}

static bool emit_due_edges(rbsp_motion_queue_t* queue,
                           const rbsp_motion_io_t* io,
                           uint64_t now_ns) {
    const uint64_t pulse_ns =
        (uint64_t)CONFIG_MOTION_STEP_PULSE_WIDTH_US * 1000ULL;
    const uint64_t minimum_low_ns =
        (uint64_t)CONFIG_MOTION_MIN_STEP_LOW_US * 1000ULL;
    for (uint8_t axis = 0U; axis < queue->axis_count; ++axis) {
        if (queue->step_high[axis] &&
            queue->active_fall_ns[axis] <= now_ns) {
            if (!io->set_step(axis, false)) {
                return false;
            }
            queue->step_high[axis] = false;
            queue->last_step_fall_ns[axis] = now_ns;
            ++queue->emitted_edges;
        }
    }
    for (uint8_t axis = 0U; axis < queue->axis_count; ++axis) {
        if (queue->active_emitted[axis] >=
                queue->active_target[axis] ||
            queue->active_next_rise_ns[axis] > now_ns) {
            continue;
        }
        if (queue->last_step_fall_ns[axis] != 0U &&
            now_ns - queue->last_step_fall_ns[axis] <
                minimum_low_ns) {
            /* 迟到已经侵占STEP低电平，宁可停机也不能输出零宽低脉冲。 */
            return false;
        }
        if (queue->step_high[axis] ||
            !io->set_step(axis, true)) {
            return false;
        }
        queue->step_high[axis] = true;
        /* GPIO 实际在本次服务中才拉高，脉宽必须从实际边沿而非名义时间计算。 */
        queue->active_fall_ns[axis] = now_ns + pulse_ns;
        ++queue->active_emitted[axis];
        ++queue->emitted_steps[axis];
        ++queue->emitted_edges;
        queue->position_steps[axis] +=
            queue->direction_positive[axis] ? 1 : -1;

        if (queue->active_emitted[axis] <
            queue->active_target[axis]) {
            queue->active_next_rise_ns[axis] +=
                queue->active_interval_ns[axis];
            queue->active_interval_error[axis] +=
                queue->active_interval_remainder[axis];
            if (queue->active_interval_error[axis] >=
                queue->active_interval_divisor[axis]) {
                ++queue->active_next_rise_ns[axis];
                queue->active_interval_error[axis] -=
                    queue->active_interval_divisor[axis];
            }
            /* 禁止中断迟到后在同一次服务中突发补步。 */
            if (queue->active_next_rise_ns[axis] <= now_ns) {
                return false;
            }
        } else {
            queue->active_next_rise_ns[axis] =
                RBSP_MOTION_NO_DEADLINE;
        }
    }
    return true;
}

bool rbsp_motion_service(rbsp_motion_queue_t* queue,
                         const rbsp_motion_io_t* io,
                         uint64_t now_ns,
                         uint64_t* next_deadline_ns) {
    if (queue == NULL || io == NULL ||
        io->set_enable == NULL || io->set_direction == NULL ||
        io->set_step == NULL) {
        return false;
    }
    if (queue->fault != RBSP_MOTION_FAULT_NONE) {
        stop_outputs(queue, io);
        queue->next_deadline_ns = RBSP_MOTION_NO_DEADLINE;
        if (next_deadline_ns != NULL) {
            *next_deadline_ns = RBSP_MOTION_NO_DEADLINE;
        }
        return true;
    }
    if (io->limit_active != NULL) {
        for (uint8_t axis = 0U;
             axis < queue->axis_count; ++axis) {
            bool active = false;
            if (!io->limit_active(axis, &active)) {
                rbsp_motion_abort(queue, RBSP_MOTION_FAULT_ABORTED);
                stop_outputs(queue, io);
                return false;
            }
            if (active && queue->state == RBSP_MOTION_RUNNING) {
                rbsp_motion_abort(queue, RBSP_MOTION_FAULT_LIMIT);
                stop_outputs(queue, io);
                return true;
            }
        }
    }
    const rbsp_motion_segment_t* segment =
        rbsp_motion_front(queue);
    if (segment == NULL) {
        stop_outputs(queue, io);
        queue->next_deadline_ns = RBSP_MOTION_NO_DEADLINE;
        if (next_deadline_ns != NULL) {
            *next_deadline_ns = RBSP_MOTION_NO_DEADLINE;
        }
        return true;
    }

    const uint64_t due = earliest_deadline(queue, segment);
    const uint64_t maximum_lateness_ns =
        (uint64_t)CONFIG_MOTION_MAX_COMPARE_LATENESS_US * 1000ULL;
    if (now_ns > due && now_ns - due > maximum_lateness_ns) {
        rbsp_motion_abort(queue, RBSP_MOTION_FAULT_TIMING);
        stop_outputs(queue, io);
        return false;
    }
    if (queue->state == RBSP_MOTION_ARMED &&
        now_ns >= segment->start_time_ns) {
        if (!start_segment(queue, io, segment)) {
            rbsp_motion_abort(queue, RBSP_MOTION_FAULT_ABORTED);
            stop_outputs(queue, io);
            return false;
        }
    }
    if (queue->state == RBSP_MOTION_RUNNING &&
        !emit_due_edges(queue, io, now_ns)) {
        rbsp_motion_abort(queue, RBSP_MOTION_FAULT_TIMING);
        stop_outputs(queue, io);
        return false;
    }

    bool all_emitted = true;
    for (uint8_t axis = 0U; axis < queue->axis_count; ++axis) {
        if (queue->active_emitted[axis] !=
                queue->active_target[axis] ||
            queue->step_high[axis]) {
            all_emitted = false;
            break;
        }
    }
    const uint64_t end_ns =
        segment->start_time_ns + segment->duration_ns;
    if (queue->state == RBSP_MOTION_RUNNING &&
        now_ns >= end_ns && all_emitted) {
        const bool final_segment = segment->final_segment;
        if (!rbsp_motion_complete_front(queue)) {
            stop_outputs(queue, io);
            return false;
        }
        if (final_segment || queue->fault != RBSP_MOTION_FAULT_NONE) {
            stop_outputs(queue, io);
        } else {
            /* 连续段在同一时间边界启动，方向建立时间在下一次上升沿前保证。 */
            segment = rbsp_motion_front(queue);
            if (segment != NULL && now_ns >= segment->start_time_ns &&
                !start_segment(queue, io, segment)) {
                rbsp_motion_abort(queue, RBSP_MOTION_FAULT_ABORTED);
                stop_outputs(queue, io);
                return false;
            }
        }
    }

    segment = rbsp_motion_front(queue);
    queue->next_deadline_ns =
        segment == NULL ? RBSP_MOTION_NO_DEADLINE
                        : earliest_deadline(queue, segment);
    if (next_deadline_ns != NULL) {
        *next_deadline_ns = queue->next_deadline_ns;
    }
    return true;
}

bool rbsp_motion_tick(rbsp_motion_queue_t* queue,
                      const rbsp_motion_io_t* io,
                      uint64_t now_ns) {
    return rbsp_motion_service(queue, io, now_ns, NULL);
}

#endif
