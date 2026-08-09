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
static uint64_t io_now_ns;

typedef struct {
    uint64_t time_ns;
    uint8_t axis;
    bool high;
} recorded_step_edge_t;

static recorded_step_edge_t recorded_step_edges[64];
static size_t recorded_step_edge_count;

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
    if (recorded_step_edge_count <
        sizeof(recorded_step_edges) / sizeof(recorded_step_edges[0])) {
        recorded_step_edges[recorded_step_edge_count++] =
            (recorded_step_edge_t){io_now_ns, axis, high};
    }
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

static bool service_at(rbsp_motion_queue_t* queue,
                       const rbsp_motion_io_t* io,
                       uint64_t now_ns,
                       uint64_t* deadline_ns) {
    io_now_ns = now_ns;
    return rbsp_motion_service(
        queue, io, now_ns, deadline_ns);
}

static void test_compare_deadlines_and_fractional_dda(void) {
    memset(io_enabled, 0, sizeof(io_enabled));
    memset(io_direction, 0, sizeof(io_direction));
    memset(io_step, 0, sizeof(io_step));
    memset(io_limit, 0, sizeof(io_limit));
    recorded_step_edge_count = 0U;
    const rbsp_motion_io_t io = {
        set_enable, set_direction, set_step, limit_active};
    rbsp_motion_queue_t queue;
    assert(rbsp_motion_init(&queue, 2U));

    rbsp_motion_segment_t segment =
        make_segment(1U, 1000000ULL, 100001ULL, true);
    segment.axis_count = 2U;
    memset(segment.steps, 0, sizeof(segment.steps));
    segment.steps[0] = 4;
    segment.steps[1] = -3;
    assert(rbsp_motion_enqueue(&queue, &segment, 0U, NULL) ==
           RBSP_MOTION_ENQUEUE_OK);

    uint64_t deadline_ns = 0U;
    assert(service_at(&queue, &io, 0U, &deadline_ns));
    assert(deadline_ns == 1000000ULL);
    while (deadline_ns != RBSP_MOTION_NO_DEADLINE) {
        const uint64_t previous = deadline_ns;
        assert(service_at(&queue, &io, previous, &deadline_ns));
        assert(deadline_ns == RBSP_MOTION_NO_DEADLINE ||
               deadline_ns > previous);
    }

    assert(queue.state == RBSP_MOTION_IDLE);
    assert(queue.position_steps[0] == 4);
    assert(queue.position_steps[1] == -3);
    assert(recorded_step_edge_count == 14U);
    rbsp_motion_status_snapshot_t snapshot;
    assert(rbsp_motion_snapshot(&queue, &snapshot));
    assert(snapshot.state == RBSP_MOTION_IDLE);
    assert(snapshot.axis_count == 2U);
    assert(snapshot.position_steps[0] == 4);
    assert(snapshot.position_steps[1] == -3);
    assert(snapshot.emitted_steps[0] == 4U);
    assert(snapshot.emitted_steps[1] == 3U);

    const uint64_t expected_axis0_rises[] = {
        1002000ULL, 1034000ULL, 1066000ULL, 1098001ULL};
    const uint64_t expected_axis1_rises[] = {
        1002000ULL, 1050000ULL, 1098001ULL};
    size_t axis0_rises = 0U;
    size_t axis1_rises = 0U;
    uint64_t last_rise[2] = {0U, 0U};
    for (size_t index = 0U;
         index < recorded_step_edge_count; ++index) {
        const recorded_step_edge_t* edge =
            &recorded_step_edges[index];
        if (edge->high) {
            if (edge->axis == 0U) {
                assert(edge->time_ns ==
                       expected_axis0_rises[axis0_rises++]);
            } else {
                assert(edge->axis == 1U);
                assert(edge->time_ns ==
                       expected_axis1_rises[axis1_rises++]);
            }
            last_rise[edge->axis] = edge->time_ns;
        } else {
            assert(last_rise[edge->axis] != 0U);
            assert(edge->time_ns - last_rise[edge->axis] ==
                   CONFIG_MOTION_STEP_PULSE_WIDTH_US * 1000ULL);
        }
    }
    assert(axis0_rises == 4U);
    assert(axis1_rises == 3U);
}

static void test_compare_lateness_stops_instead_of_bursting(void) {
    memset(io_enabled, 0, sizeof(io_enabled));
    memset(io_direction, 0, sizeof(io_direction));
    memset(io_step, 0, sizeof(io_step));
    memset(io_limit, 0, sizeof(io_limit));
    recorded_step_edge_count = 0U;
    const rbsp_motion_io_t io = {
        set_enable, set_direction, set_step, limit_active};
    rbsp_motion_queue_t queue;
    assert(rbsp_motion_init(&queue, 1U));
    rbsp_motion_segment_t segment =
        make_segment(1U, 1000000ULL, 100000ULL, true);
    segment.axis_count = 1U;
    memset(segment.steps, 0, sizeof(segment.steps));
    segment.steps[0] = 2;
    assert(rbsp_motion_enqueue(&queue, &segment, 0U, NULL) ==
           RBSP_MOTION_ENQUEUE_OK);

    uint64_t deadline_ns = 0U;
    assert(service_at(&queue, &io, 1000000ULL, &deadline_ns));
    assert(deadline_ns == 1002000ULL);
    const uint64_t too_late = deadline_ns +
        (uint64_t)(CONFIG_MOTION_MAX_COMPARE_LATENESS_US + 1U) *
            1000ULL;
    assert(!service_at(&queue, &io, too_late, &deadline_ns));
    assert(queue.state == RBSP_MOTION_FAULTED);
    assert(queue.fault == RBSP_MOTION_FAULT_TIMING);
    assert(queue.emitted_steps[0] == 0U);
    assert(!io_enabled[0]);
    assert(!io_step[0]);
}

static void test_total_step_rate_budget(void) {
    rbsp_motion_queue_t queue;
    assert(rbsp_motion_init(&queue, CONFIG_MOTION_MAX_AXES));
    rbsp_motion_segment_t segment =
        make_segment(1U, 1000000ULL, 1000000ULL, true);
    for (uint8_t axis = 0U;
         axis < CONFIG_MOTION_MAX_AXES; ++axis) {
        segment.steps[axis] = 60;
    }
    assert(rbsp_motion_enqueue(&queue, &segment, 0U, NULL) ==
           RBSP_MOTION_ENQUEUE_INVALID);
}

static void test_append_does_not_restart_running_segment(void) {
    memset(io_enabled, 0, sizeof(io_enabled));
    memset(io_direction, 0, sizeof(io_direction));
    memset(io_step, 0, sizeof(io_step));
    memset(io_limit, 0, sizeof(io_limit));
    const rbsp_motion_io_t io = {
        set_enable, set_direction, set_step, limit_active};
    rbsp_motion_queue_t queue;
    assert(rbsp_motion_init(&queue, 1U));

    rbsp_motion_segment_t first =
        make_segment(1U, 1000000ULL, 2000000ULL, false);
    first.axis_count = 1U;
    memset(first.steps, 0, sizeof(first.steps));
    first.steps[0] = 2;
    assert(rbsp_motion_enqueue(&queue, &first, 0U, NULL) ==
           RBSP_MOTION_ENQUEUE_OK);
    uint64_t deadline_ns = 0U;
    assert(service_at(&queue, &io, 1000000ULL, &deadline_ns));
    assert(queue.state == RBSP_MOTION_RUNNING);

    rbsp_motion_segment_t second =
        make_segment(2U, 0U, 100000ULL, true);
    second.axis_count = 1U;
    memset(second.steps, 0, sizeof(second.steps));
    second.steps[0] = 1;
    assert(rbsp_motion_enqueue(&queue, &second, 1000000ULL, NULL) ==
           RBSP_MOTION_ENQUEUE_OK);
    assert(queue.state == RBSP_MOTION_RUNNING);
    assert(queue.size == 2U);
    assert(queue.active_target[0] == 2U);
    assert(queue.next_deadline_ns == deadline_ns);
}

static void test_shared_enable_group(void) {
    const uint16_t groups[] = {10U, 10U, 20U};
    bool requests[] = {false, false, false};
    bool physical = false;
    assert(rbsp_motion_shared_enable_update(
        groups, requests, 3U, 0U, true, &physical));
    assert(physical);
    assert(rbsp_motion_shared_enable_update(
        groups, requests, 3U, 1U, true, &physical));
    assert(physical);
    assert(rbsp_motion_shared_enable_update(
        groups, requests, 3U, 0U, false, &physical));
    assert(physical);
    assert(rbsp_motion_shared_enable_update(
        groups, requests, 3U, 1U, false, &physical));
    assert(!physical);
    assert(rbsp_motion_shared_enable_update(
        groups, requests, 3U, 2U, true, &physical));
    assert(physical);
    assert(!rbsp_motion_shared_enable_update(
        groups, requests, 3U, 3U, true, &physical));
}

int main(void) {
    test_auto_append_and_completion();
    test_validation_and_underrun();
    test_capacity_and_abort();
    test_runtime_axis_count_is_smaller_than_capacity();
    test_timer_executor_and_limit();
    test_timer_executor_underrun_stops_outputs();
    test_compare_deadlines_and_fractional_dda();
    test_compare_lateness_stops_instead_of_bursting();
    test_total_step_rate_budget();
    test_append_does_not_restart_running_segment();
    test_shared_enable_group();
}
