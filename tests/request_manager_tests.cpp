#include "remotebsp/toolbusd/request_manager.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>

using namespace remotebsp;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": 检查失败: " #condition "\n";                        \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

protocol::Packet make_request(std::uint32_t session_id = 7) {
    protocol::Packet request;
    request.header.message_type = protocol::MessageType::Request;
    request.header.command =
        static_cast<std::uint16_t>(protocol::Command::Ping);
    request.header.session_id = session_id;
    request.header.object_id = 3;
    request.payload = {1, 2, 3};
    return request;
}

protocol::Packet make_response(const protocol::Packet& request) {
    protocol::Packet response;
    response.header.message_type = protocol::MessageType::Response;
    response.header.command = request.header.command;
    response.header.session_id = request.header.session_id;
    response.header.request_id = request.header.request_id;
    response.header.object_id = request.header.object_id;
    response.payload = {0};
    return response;
}

void test_id_and_matching() {
    toolbusd::RequestManager manager;
    const auto now = toolbusd::RequestManager::TimePoint{};
    const auto first = manager.submit(make_request(), now);
    const auto second = manager.submit(make_request(), now);
    CHECK(first.request_id == 1);
    CHECK(second.request_id == 2);
    CHECK(manager.pending_count() == 2);

    const auto matched =
        manager.accept_response(make_response(first.packet), now);
    CHECK(matched.status == toolbusd::ResponseStatus::Matched);
    CHECK(matched.response.has_value());
    CHECK(manager.pending_count() == 1);

    const auto duplicate =
        manager.accept_response(make_response(first.packet), now);
    CHECK(duplicate.status == toolbusd::ResponseStatus::Duplicate);

    auto wrong = make_response(second.packet);
    wrong.header.command =
        static_cast<std::uint16_t>(protocol::Command::GetInfo);
    CHECK(manager.accept_response(wrong, now).status ==
          toolbusd::ResponseStatus::Unexpected);
    CHECK(manager.pending_count() == 1);
}

void test_retry_and_timeout() {
    toolbusd::RequestManagerConfig config;
    config.timeout = std::chrono::milliseconds(100);
    config.maximum_retries = 2;
    config.duplicate_window = std::chrono::milliseconds(500);
    toolbusd::RequestManager manager(config);
    const auto start = toolbusd::RequestManager::TimePoint{};
    const auto submission = manager.submit(make_request(), start);

    CHECK(manager.poll(start + std::chrono::milliseconds(99)).empty());
    auto events = manager.poll(start + std::chrono::milliseconds(100));
    CHECK(events.size() == 1);
    CHECK(events[0].type == toolbusd::RequestEventType::Retry);
    CHECK(events[0].packet.has_value());
    CHECK(events[0].packet->header.request_id == submission.request_id);

    events = manager.poll(start + std::chrono::milliseconds(200));
    CHECK(events.size() == 1);
    CHECK(events[0].type == toolbusd::RequestEventType::Retry);

    events = manager.poll(start + std::chrono::milliseconds(300));
    CHECK(events.size() == 1);
    CHECK(events[0].type == toolbusd::RequestEventType::TimedOut);
    CHECK(!events[0].packet.has_value());
    CHECK(manager.pending_count() == 0);

    CHECK(manager.accept_response(make_response(submission.packet),
                                  start + std::chrono::milliseconds(301))
              .status == toolbusd::ResponseStatus::Duplicate);
    manager.poll(start + std::chrono::milliseconds(800));
    CHECK(manager.completed_count() == 0);
    CHECK(manager.accept_response(make_response(submission.packet),
                                  start + std::chrono::milliseconds(801))
              .status == toolbusd::ResponseStatus::Unexpected);
}

void test_session_and_response_validation() {
    toolbusd::RequestManager manager;
    const auto now = toolbusd::RequestManager::TimePoint{};
    const auto request = manager.submit(make_request(10), now);

    auto wrong_session = make_response(request.packet);
    wrong_session.header.session_id = 11;
    CHECK(manager.accept_response(wrong_session, now).status ==
          toolbusd::ResponseStatus::Unexpected);

    auto event = make_response(request.packet);
    event.header.message_type = protocol::MessageType::Event;
    CHECK(manager.accept_response(event, now).status ==
          toolbusd::ResponseStatus::Unexpected);
    CHECK(manager.pending_count() == 1);

    auto create = make_request(10);
    create.header.command =
        static_cast<std::uint16_t>(protocol::Command::GpioCreate);
    create.header.object_id = 0;
    const auto create_submission = manager.submit(create, now);
    auto create_response = make_response(create_submission.packet);
    create_response.header.object_id = 42;
    CHECK(manager.accept_response(create_response, now).status ==
          toolbusd::ResponseStatus::Matched);

    const auto failed_create = manager.submit(create, now);
    auto failed_response = make_response(failed_create.packet);
    failed_response.header.flags = protocol::kErrorResponseFlag;
    failed_response.payload = {2};
    CHECK(manager.accept_response(failed_response, now).status ==
          toolbusd::ResponseStatus::Matched);

    auto uart_create = make_request(10);
    uart_create.header.command =
        static_cast<std::uint16_t>(protocol::Command::UartCreate);
    uart_create.header.object_id = 0;
    const auto uart_submission = manager.submit(uart_create, now);
    auto uart_response = make_response(uart_submission.packet);
    uart_response.header.object_id = 43;
    CHECK(manager.accept_response(uart_response, now).status ==
          toolbusd::ResponseStatus::Matched);

    for (const auto command : {protocol::Command::PwmCreate,
                               protocol::Command::TimedBitstreamCreate}) {
        auto waveform_create = make_request(10);
        waveform_create.header.command =
            static_cast<std::uint16_t>(command);
        waveform_create.header.object_id = 0;
        const auto waveform_submission =
            manager.submit(waveform_create, now);
        auto waveform_response = make_response(waveform_submission.packet);
        waveform_response.header.object_id = 44;
        CHECK(manager.accept_response(waveform_response, now).status ==
              toolbusd::ResponseStatus::Matched);
    }

    uart_create.header.object_id = 99;
    const auto invalid_submission = manager.submit(uart_create, now);
    auto invalid_response = make_response(invalid_submission.packet);
    invalid_response.header.flags = protocol::kErrorResponseFlag;
    invalid_response.payload = {2};
    CHECK(manager.accept_response(invalid_response, now).status ==
          toolbusd::ResponseStatus::Matched);
}

void test_cancel_unsent_request() {
    toolbusd::RequestManager manager;
    const auto submission = manager.submit(make_request());
    CHECK(manager.pending_count() == 1);
    CHECK(manager.cancel(submission.packet.header.session_id,
                         submission.request_id));
    CHECK(manager.pending_count() == 0);
    CHECK(!manager.cancel(submission.packet.header.session_id,
                          submission.request_id));
    CHECK(manager.completed_count() == 0);
}

}

int main() {
    test_id_and_matching();
    test_retry_and_timeout();
    test_session_and_response_validation();
    test_cancel_unsent_request();
    if (failures != 0) {
        std::cerr << failures << " 个测试失败\n";
        return 1;
    }
    std::cout << "所有请求管理器测试通过\n";
    return 0;
}
