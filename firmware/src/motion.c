#include "remotebsp_embedded/motion.h"

#if defined(CONFIG_REMOTEBSP_MOTION)

#include <limits.h>
#include <stddef.h>
#include <string.h>

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

static uint64_t maximum_steps_for_duration(uint64_t duration_ns) {
    const uint64_t whole_seconds = duration_ns / 1000000000ULL;
    const uint64_t remainder_ns = duration_ns % 1000000000ULL;
    const uint64_t half_rate = CONFIG_MOTION_TIMER_HZ / 2U;
    if (whole_seconds > UINT64_MAX / half_rate) {
        return UINT64_MAX;
    }
    const uint64_t whole = whole_seconds * half_rate;
    const uint64_t partial =
        (remainder_ns * CONFIG_MOTION_TIMER_HZ) /
        2000000000ULL;
    return UINT64_MAX - whole < partial
               ? UINT64_MAX
               : whole + partial;
}

bool rbsp_motion_init(rbsp_motion_queue_t* queue, uint8_t axis_count) {
    if (queue == NULL || axis_count == 0U ||
        axis_count > CONFIG_MOTION_MAX_AXES) {
        return false;
    }
    memset(queue, 0, sizeof(*queue));
    queue->axis_count = axis_count;
    queue->state = RBSP_MOTION_IDLE;
    return true;
}

rbsp_motion_enqueue_result_t rbsp_motion_enqueue(
    rbsp_motion_queue_t* queue, const rbsp_motion_segment_t* requested,
    uint64_t now_ns, rbsp_motion_segment_t* accepted) {
    if (queue == NULL || requested == NULL ||
        requested->sequence == 0U || requested->duration_ns == 0U ||
        requested->axis_count != queue->axis_count ||
        UINT64_MAX - requested->start_time_ns <
            requested->duration_ns) {
        return RBSP_MOTION_ENQUEUE_INVALID;
    }
    const uint64_t maximum_steps =
        maximum_steps_for_duration(requested->duration_ns);
    for (uint8_t axis = 0U;
         axis < queue->axis_count; ++axis) {
        const uint64_t steps =
            absolute_steps(requested->steps[axis]);
        if (steps > maximum_steps ||
            (steps != 0U &&
             requested->duration_ns > UINT64_MAX / steps)) {
            ++queue->rejected_segments;
            return RBSP_MOTION_ENQUEUE_INVALID;
        }
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
    queue->state = RBSP_MOTION_ARMED;
    ++queue->accepted_segments;
    if (queue->size > queue->maximum_queue_depth) {
        queue->maximum_queue_depth = queue->size;
    }
    if (accepted != NULL) {
        *accepted = value;
    }
    return RBSP_MOTION_ENQUEUE_OK;
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

bool rbsp_motion_tick(rbsp_motion_queue_t* queue,
                      const rbsp_motion_io_t* io,
                      uint64_t now_ns) {
    if (queue == NULL || io == NULL ||
        io->set_enable == NULL || io->set_direction == NULL ||
        io->set_step == NULL) {
        return false;
    }
    if (queue->fault != RBSP_MOTION_FAULT_NONE) {
        stop_outputs(queue, io);
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
        return true;
    }
    if (queue->state == RBSP_MOTION_ARMED) {
        if (now_ns < segment->start_time_ns) {
            return true;
        }
        memset(queue->active_emitted, 0,
               sizeof(queue->active_emitted));
        memset(queue->active_phase, 0, sizeof(queue->active_phase));
        queue->active_last_tick_ns = segment->start_time_ns;
        for (uint8_t axis = 0U;
             axis < queue->axis_count; ++axis) {
            const bool positive = segment->steps[axis] >= 0;
            if (!io->set_direction(axis, positive)) {
                rbsp_motion_abort(
                    queue, RBSP_MOTION_FAULT_ABORTED);
                stop_outputs(queue, io);
                return false;
            }
            queue->direction_positive[axis] = positive;
        }
        if (!set_segment_enabled(queue, io, segment)) {
            rbsp_motion_abort(queue, RBSP_MOTION_FAULT_ABORTED);
            stop_outputs(queue, io);
            return false;
        }
        queue->state = RBSP_MOTION_RUNNING;
    }

    bool all_emitted = true;
    const uint64_t elapsed =
        now_ns <= segment->start_time_ns
            ? 0U
            : now_ns - segment->start_time_ns;
    const uint64_t bounded_elapsed =
        elapsed < segment->duration_ns
            ? elapsed
            : segment->duration_ns;
    const uint64_t tick_time = segment->start_time_ns + bounded_elapsed;
    const uint64_t delta_ns =
        tick_time > queue->active_last_tick_ns
            ? tick_time - queue->active_last_tick_ns
            : 0U;
    queue->active_last_tick_ns = tick_time;
    for (uint8_t axis = 0U;
         axis < queue->axis_count; ++axis) {
        if (queue->step_high[axis]) {
            if (!io->set_step(axis, false)) {
                rbsp_motion_abort(
                    queue, RBSP_MOTION_FAULT_ABORTED);
                stop_outputs(queue, io);
                return false;
            }
            queue->step_high[axis] = false;
            ++queue->emitted_edges;
        }
        const uint64_t target =
            absolute_steps(segment->steps[axis]);
        /*
         * 均匀 DDA：相位每个 tick 增加 target × delta_ns，达到一个
         * segment.duration_ns 就发出一个 STEP。即使中断有短暂延迟，
         * 未发出的相位也会保留，不能静默丢步。
         */
        if (target != 0U && delta_ns != 0U) {
            const uint64_t increment = target * delta_ns;
            if (UINT64_MAX - queue->active_phase[axis] < increment) {
                rbsp_motion_abort(queue, RBSP_MOTION_FAULT_UNDERRUN);
                stop_outputs(queue, io);
                return false;
            }
            queue->active_phase[axis] += increment;
        }
        if (queue->active_emitted[axis] < target &&
            queue->active_phase[axis] >= segment->duration_ns) {
            queue->active_phase[axis] -= segment->duration_ns;
            if (!io->set_step(axis, true)) {
                rbsp_motion_abort(
                    queue, RBSP_MOTION_FAULT_ABORTED);
                stop_outputs(queue, io);
                return false;
            }
            queue->step_high[axis] = true;
            ++queue->active_emitted[axis];
            ++queue->emitted_steps[axis];
            ++queue->emitted_edges;
            queue->position_steps[axis] +=
                queue->direction_positive[axis] ? 1 : -1;
        }
        if (queue->active_emitted[axis] != target ||
            queue->step_high[axis]) {
            all_emitted = false;
        }
    }
    if (elapsed >= segment->duration_ns && all_emitted) {
        const bool final_segment = segment->final_segment;
        if (!rbsp_motion_complete_front(queue)) {
            return false;
        }
        if (final_segment || queue->fault != RBSP_MOTION_FAULT_NONE) {
            stop_outputs(queue, io);
        }
    } else if (elapsed > segment->duration_ns +
                           1000000000ULL /
                               CONFIG_MOTION_TIMER_HZ) {
        rbsp_motion_abort(queue, RBSP_MOTION_FAULT_UNDERRUN);
        ++queue->queue_underruns;
        stop_outputs(queue, io);
    }
    return true;
}

#endif
