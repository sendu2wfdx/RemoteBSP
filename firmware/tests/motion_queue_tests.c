#include "remotebsp_embedded/motion.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static rbsp_motion_segment_t make_segment(
    uint32_t sequence, uint64_t start_time_ns,
    uint64_t duration_ns, bool final_segment) {
    rbsp_motion_segment_t segment;
    memset(&segment, 0, sizeof(segment));
    segment.sequence = sequence;
    segment.start_time_ns = start_time_ns;
    segment.duration_ns = duration_ns;
    segment.axis_count = CONFIG_MOTION_MAX_AXES;
    segment.final_segment = final_segment;
    for (uint8_t index = 0U;
         index < CONFIG_MOTION_MAX_AXES; ++index) {
        segment.steps[index] = (int32_t)index + 1;
    }
    return segment;
}

static void test_auto_append_and_completion(void) {
    rbsp_motion_queue_t queue;
    assert(rbsp_motion_init(&queue, CONFIG_MOTION_MAX_AXES));

    rbsp_motion_segment_t accepted_first;
    const rbsp_motion_segment_t first =
        make_segment(1U, 0U, 2000000ULL, false);
    assert(rbsp_motion_enqueue(
               &queue, &first, 0U, &accepted_first) ==
           RBSP_MOTION_ENQUEUE_OK);
    assert(accepted_first.start_time_ns == 1000000ULL);

    rbsp_motion_segment_t accepted_second;
    const rbsp_motion_segment_t second =
        make_segment(2U, 0U, 3000000ULL, true);
    assert(rbsp_motion_enqueue(
               &queue, &second, 0U, &accepted_second) ==
           RBSP_MOTION_ENQUEUE_OK);
    assert(accepted_second.start_time_ns == 3000000ULL);
    assert(queue.size == 2U);
    assert(rbsp_motion_front(&queue)->sequence == 1U);

    assert(rbsp_motion_complete_front(&queue));
    assert(queue.fault == RBSP_MOTION_FAULT_NONE);
    assert(rbsp_motion_front(&queue)->sequence == 2U);
    assert(rbsp_motion_complete_front(&queue));
    assert(queue.state == RBSP_MOTION_IDLE);
    assert(queue.last_completed_sequence == 2U);
}

static void test_validation_and_underrun(void) {
    rbsp_motion_queue_t queue;
    assert(rbsp_motion_init(&queue, CONFIG_MOTION_MAX_AXES));

    rbsp_motion_segment_t invalid =
        make_segment(2U, 1000000ULL, 1000000ULL, false);
    assert(rbsp_motion_enqueue(&queue, &invalid, 0U, NULL) ==
           RBSP_MOTION_ENQUEUE_SEQUENCE);

    invalid.sequence = 1U;
    invalid.axis_count = 3U;
    assert(rbsp_motion_enqueue(&queue, &invalid, 0U, NULL) ==
           RBSP_MOTION_ENQUEUE_INVALID);

    const rbsp_motion_segment_t only =
        make_segment(1U, 1000000ULL, 1000000ULL, false);
    assert(rbsp_motion_enqueue(&queue, &only, 0U, NULL) ==
           RBSP_MOTION_ENQUEUE_OK);
    assert(rbsp_motion_complete_front(&queue));
    assert(queue.state == RBSP_MOTION_FAULTED);
    assert(queue.fault == RBSP_MOTION_FAULT_UNDERRUN);
    assert(rbsp_motion_clear_fault(&queue, 2000000ULL));
    assert(queue.state == RBSP_MOTION_IDLE);
}

static void test_capacity_and_abort(void) {
    rbsp_motion_queue_t queue;
    assert(rbsp_motion_init(&queue, CONFIG_MOTION_MAX_AXES));
    for (uint32_t sequence = 1U; sequence <= 3U; ++sequence) {
        const rbsp_motion_segment_t segment =
            make_segment(sequence, 0U, 1000000ULL, false);
        assert(rbsp_motion_enqueue(&queue, &segment, 0U, NULL) ==
               RBSP_MOTION_ENQUEUE_OK);
    }
    const rbsp_motion_segment_t overflow =
        make_segment(4U, 0U, 1000000ULL, true);
    assert(rbsp_motion_enqueue(&queue, &overflow, 0U, NULL) ==
           RBSP_MOTION_ENQUEUE_FULL);

    rbsp_motion_abort(&queue, RBSP_MOTION_FAULT_LIMIT);
    assert(queue.size == 0U);
    assert(queue.state == RBSP_MOTION_FAULTED);
    assert(queue.fault == RBSP_MOTION_FAULT_LIMIT);
}

static void test_runtime_axis_count_is_smaller_than_capacity(void) {
    rbsp_motion_queue_t queue;
    assert(!rbsp_motion_init(&queue, 0U));
    assert(rbsp_motion_init(&queue, 2U));
    assert(queue.axis_count == 2U);

    rbsp_motion_segment_t segment =
        make_segment(1U, 1000000ULL, 1000000ULL, true);
    segment.axis_count = 2U;
    segment.steps[2] = 0;
    segment.steps[3] = 0;
    assert(rbsp_motion_enqueue(&queue, &segment, 0U, NULL) ==
           RBSP_MOTION_ENQUEUE_OK);

    rbsp_motion_queue_t invalid_queue;
    assert(rbsp_motion_init(&invalid_queue, 2U));
    segment.axis_count = 3U;
    assert(rbsp_motion_enqueue(&invalid_queue, &segment, 0U, NULL) ==
           RBSP_MOTION_ENQUEUE_INVALID);
}

static bool io_enabled[CONFIG_MOTION_MAX_AXES];
static bool io_direction[CONFIG_MOTION_MAX_AXES];
static bool io_step[CONFIG_MOTION_MAX_AXES];
static bool io_limit[CONFIG_MOTION_MAX_AXES];

static bool set_enable(uint8_t axis, bool enabled) {
    io_enabled[axis] = enabled;
    return true;
}

static bool set_direction(uint8_t axis, bool positive) {
    io_direction[axis] = positive;
    return true;
}

static bool set_step(uint8_t axis, bool high) {
    io_step[axis] = high;
    return true;
}

static bool limit_active(uint8_t axis, bool* active) {
    *active = io_limit[axis];
    return true;
}

static void test_timer_executor_and_limit(void) {
    memset(io_enabled, 0, sizeof(io_enabled));
    memset(io_direction, 0, sizeof(io_direction));
    memset(io_step, 0, sizeof(io_step));
    memset(io_limit, 0, sizeof(io_limit));
    const rbsp_motion_io_t io = {
        set_enable, set_direction, set_step, limit_active};
    rbsp_motion_queue_t queue;
    assert(rbsp_motion_init(&queue, CONFIG_MOTION_MAX_AXES));
    rbsp_motion_segment_t segment =
        make_segment(1U, 1000000ULL, 100000ULL, true);
    memset(segment.steps, 0, sizeof(segment.steps));
    segment.steps[0] = 2;
    segment.steps[1] = -1;
    assert(rbsp_motion_enqueue(
               &queue, &segment, 0U, NULL) ==
           RBSP_MOTION_ENQUEUE_OK);
    assert(rbsp_motion_tick(&queue, &io, 999999ULL));
    assert(queue.state == RBSP_MOTION_ARMED);
    assert(rbsp_motion_tick(&queue, &io, 1050000ULL));
    assert(queue.state == RBSP_MOTION_RUNNING);
    assert(io_step[0]);
    assert(queue.position_steps[0] == 1);
    assert(!io_direction[1]);
    assert(rbsp_motion_tick(&queue, &io, 1060000ULL));
    assert(!io_step[0]);
    assert(rbsp_motion_tick(&queue, &io, 1100000ULL));
    assert(io_step[0]);
    assert(rbsp_motion_tick(&queue, &io, 1110000ULL));
    assert(queue.state == RBSP_MOTION_IDLE);
    assert(queue.position_steps[0] == 2);
    assert(queue.position_steps[1] == -1);
    assert(queue.emitted_steps[0] == 2);
    assert(queue.completed_segments == 1);
    assert(!io_enabled[0]);

    assert(rbsp_motion_init(&queue, CONFIG_MOTION_MAX_AXES));
    segment.sequence = 1U;
    segment.start_time_ns = 2000000ULL;
    segment.final_segment = true;
    assert(rbsp_motion_enqueue(
               &queue, &segment, 0U, NULL) ==
           RBSP_MOTION_ENQUEUE_OK);
    assert(rbsp_motion_tick(&queue, &io, 2050000ULL));
    io_limit[0] = true;
    assert(rbsp_motion_tick(&queue, &io, 2060000ULL));
    assert(queue.state == RBSP_MOTION_FAULTED);
    assert(queue.fault == RBSP_MOTION_FAULT_LIMIT);
    assert(queue.limit_stops == 1);
    assert(!io_enabled[0]);
}

static void test_timer_executor_underrun_stops_outputs(void) {
    memset(io_enabled, 0, sizeof(io_enabled));
    memset(io_direction, 0, sizeof(io_direction));
    memset(io_step, 0, sizeof(io_step));
    memset(io_limit, 0, sizeof(io_limit));
    const rbsp_motion_io_t io = {
        set_enable, set_direction, set_step, limit_active};
    rbsp_motion_queue_t queue;
    assert(rbsp_motion_init(&queue, CONFIG_MOTION_MAX_AXES));
    rbsp_motion_segment_t segment =
        make_segment(1U, 1000000ULL, 100000ULL, false);
    memset(segment.steps, 0, sizeof(segment.steps));
    segment.steps[0] = 2;
    assert(rbsp_motion_enqueue(
               &queue, &segment, 0U, NULL) ==
           RBSP_MOTION_ENQUEUE_OK);
    assert(rbsp_motion_tick(&queue, &io, 1050000ULL));
    assert(io_enabled[0]);
    assert(rbsp_motion_tick(&queue, &io, 1060000ULL));
    assert(rbsp_motion_tick(&queue, &io, 1100000ULL));
    assert(rbsp_motion_tick(&queue, &io, 1110000ULL));
    assert(queue.state == RBSP_MOTION_FAULTED);
    assert(queue.fault == RBSP_MOTION_FAULT_UNDERRUN);
    assert(queue.queue_underruns == 1U);
    assert(queue.safety_stops == 1U);
    assert(!io_enabled[0]);
    assert(!io_step[0]);
}

int main(void) {
    test_auto_append_and_completion();
    test_validation_and_underrun();
    test_capacity_and_abort();
    test_runtime_axis_count_is_smaller_than_capacity();
    test_timer_executor_and_limit();
    test_timer_executor_underrun_stops_outputs();
}
