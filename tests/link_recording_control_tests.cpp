#include "remotebsp/toolbusd/link_recording.hpp"

#include <cassert>
#include <filesystem>
#include <optional>
#include <unistd.h>

namespace fs = std::filesystem;

class Loopback final : public remotebsp::transport::LinkTransport {
public:
    void send(const remotebsp::transport::LinkFrame& frame) override { pending = frame; }
    std::optional<remotebsp::transport::LinkFrame> receive(
        std::chrono::milliseconds) override {
        auto result = pending; pending.reset(); return result;
    }
    remotebsp::transport::LinkCapabilities capabilities() const noexcept override {
        return {remotebsp::transport::LinkKind::MockUsb, 64U, true, true, true, 0U};
    }
    std::optional<remotebsp::transport::LinkFrame> pending;
};

int main() {
    const auto directory = fs::temp_directory_path() /
        ("rbsp-logical-recording-" + std::to_string(::getpid()));
    fs::create_directory(directory);
    remotebsp::toolbusd::LinkRecordingController controller(directory.string());
    assert(!controller.status().active);
    auto link = controller.start(std::make_unique<Loopback>(), "one.rbsplog");
    link->send({0x501U, {1U, 2U, 3U}});
    assert(link->receive(std::chrono::milliseconds(1)).has_value());
    assert(controller.status().active && controller.status().event_count == 2U);
    const auto path = controller.stop();
    const auto replay = remotebsp::toolbusd::LinkRecordingController::consume_offline(
        directory.string(), "one.rbsplog");
    assert(replay.event_count == 2U);
    bool rejected = false;
    try { (void)controller.start(std::make_unique<Loopback>(), "../escape.rbsplog"); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { (void)controller.start(std::make_unique<Loopback>(), "one.rbsplog"); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    fs::remove(path); fs::remove(directory);
}
