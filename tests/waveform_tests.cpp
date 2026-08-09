#include "remotebsp/mock_mcu/remote_core.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

using namespace remotebsp;

namespace {

protocol::Packet request(protocol::Command command,
                         std::uint32_t request_id,
                         std::uint32_t object_id = 0) {
    protocol::Packet value;
    value.header.message_type = protocol::MessageType::Request;
    value.header.command = static_cast<std::uint16_t>(command);
    value.header.session_id = 1;
    value.header.request_id = request_id;
    value.header.object_id = object_id;
    return value;
}

void status(const protocol::Packet& response,
            mock_mcu::StatusCode expected) {
    assert(!response.payload.empty());
    assert(response.payload[0] == static_cast<std::uint8_t>(expected));
}

mock_mcu::RemoteCore make_core(
    const std::shared_ptr<mock_mcu::WaveformBsp>& waveform) {
    const std::vector<protocol::ResourceDescriptor> resources{
        {0x06000000, protocol::ResourceType::Pwm, 0,
         protocol::kResourceFlagNative, 0, 0},
        {0x0A000000, protocol::ResourceType::TimedBitstream, 0,
         protocol::kResourceFlagNative, 0, 192}};
    const auto capabilities =
        mock_mcu::capability_mask(mock_mcu::Capability::Pwm) |
        mock_mcu::capability_mask(
            mock_mcu::Capability::TimedBitstream);
    return mock_mcu::RemoteCore(
        {}, capabilities, nullptr, nullptr, resources, {}, nullptr,
        waveform);
}

void test_pwm() {
    auto waveform = std::make_shared<mock_mcu::WaveformBsp>();
    auto core = make_core(waveform);
    auto create = request(protocol::Command::PwmCreate, 1);
    create.payload = protocol::encode_pwm_create({0, 20000, 2500, false});
    const auto created = core.handle(create);
    status(created, mock_mcu::StatusCode::Ok);
    assert(created.header.object_id != 0);
    auto snapshot = waveform->pwm_snapshot();
    assert(snapshot.size() == 1);
    assert(snapshot[0].frequency_hz == 20000);
    assert(snapshot[0].duty == 2500);

    auto write = request(protocol::Command::PwmWrite, 2,
                         created.header.object_id);
    write.payload = protocol::encode_pwm_duty(9000);
    status(core.handle(write), mock_mcu::StatusCode::Ok);
    assert(waveform->pwm_snapshot()[0].duty == 9000);

    status(core.handle(request(protocol::Command::PwmStop, 3,
                               created.header.object_id)),
           mock_mcu::StatusCode::Ok);
    assert(!waveform->pwm_snapshot()[0].running);

    auto duplicate = request(protocol::Command::PwmCreate, 4);
    duplicate.payload = protocol::encode_pwm_create({0, 1000, 0, false});
    status(core.handle(duplicate), mock_mcu::StatusCode::ResourceBusy);
}

void test_timed_bitstream() {
    auto waveform = std::make_shared<mock_mcu::WaveformBsp>();
    auto core = make_core(waveform);
    auto create = request(protocol::Command::TimedBitstreamCreate, 1);
    create.payload = protocol::encode_timed_bitstream_create(
        {0, 1250, 350, 700, 80});
    const auto created = core.handle(create);
    status(created, mock_mcu::StatusCode::Ok);

    auto write = request(protocol::Command::TimedBitstreamWrite, 2,
                         created.header.object_id);
    write.payload = protocol::encode_timed_bitstream_write(
        {48, {0x20, 0x10, 0x30, 0x50, 0x40, 0x60}});
    status(core.handle(write), mock_mcu::StatusCode::Ok);
    const auto snapshot = waveform->bitstream_snapshot();
    assert(snapshot.size() == 1);
    assert(snapshot[0].bit_count == 48);
    assert(snapshot[0].write_count == 1);
    assert(snapshot[0].data[0] == 0x20);

    status(core.handle(request(protocol::Command::TimedBitstreamAbort, 3,
                               created.header.object_id)),
           mock_mcu::StatusCode::Ok);
}

void test_unsupported() {
    mock_mcu::RemoteCore core({}, 0);
    auto create = request(protocol::Command::PwmCreate, 1);
    create.payload = protocol::encode_pwm_create({0, 1000, 0, false});
    status(core.handle(create),
           mock_mcu::StatusCode::UnsupportedCapability);
}

}

int main() {
    test_pwm();
    test_timed_bitstream();
    test_unsupported();
}
