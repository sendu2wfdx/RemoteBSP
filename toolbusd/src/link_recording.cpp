#include "remotebsp/toolbusd/link_recording.hpp"

#include <filesystem>
#include <stdexcept>

namespace remotebsp::toolbusd {
namespace fs = std::filesystem;

std::string LinkRecordingController::safe_path(
    const std::string& directory, const std::string& name) {
    if (directory.empty() || name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos ||
        fs::path(name).extension() != ".rbsplog") {
        throw std::invalid_argument(
            "逻辑链路录制文件必须是简单名称并以 .rbsplog 结尾");
    }
    const fs::path root = fs::weakly_canonical(directory);
    if (!fs::is_directory(root)) {
        throw std::invalid_argument("逻辑链路录制固定目录不存在");
    }
    return (root / name).string();
}

LinkRecordingController::LinkRecordingController(std::string fixed_directory)
    : directory_(std::move(fixed_directory)) {
    (void)safe_path(directory_, "probe.rbsplog");
}

LinkRecordingController::~LinkRecordingController() {
    if (active_) {
        try { (void)stop(); } catch (...) {}
    }
}

std::unique_ptr<transport::LinkTransport> LinkRecordingController::start(
    std::unique_ptr<transport::LinkTransport> inner,
    const std::string& output_name) {
    if (!inner || active_) {
        throw std::logic_error("逻辑链路录制已启动或底层链路无效");
    }
    const auto path = safe_path(directory_, output_name);
    if (fs::exists(path)) {
        throw std::runtime_error("逻辑链路录制目标已存在（no-clobber）");
    }
    recorder_ = std::make_shared<mock_mcu::TransportSessionRecorder>();
    output_name_ = output_name;
    active_ = true;
    return std::make_unique<mock_mcu::RecordingLinkTransport>(
        std::move(inner), mock_mcu::RecordingTransportRole::Host, recorder_);
}

std::string LinkRecordingController::stop() {
    if (!active_ || !recorder_) {
        throw std::logic_error("逻辑链路录制尚未启动");
    }
    const auto path = safe_path(directory_, output_name_);
    const auto session = recorder_->snapshot();
    mock_mcu::write_transport_replay_session(session, path);
    active_ = false;
    recorder_.reset();
    output_name_.clear();
    return path;
}

LinkRecordingStatus LinkRecordingController::status() const {
    LinkRecordingStatus result;
    result.active = active_;
    result.output_name = output_name_;
    if (recorder_) result.event_count = recorder_->snapshot().events.size();
    return result;
}

mock_mcu::TransportReplayRecord LinkRecordingController::consume_offline(
    const std::string& directory, const std::string& output_name) {
    const auto session = mock_mcu::load_transport_replay_session(
        safe_path(directory, output_name));
    mock_mcu::verify_transport_replay(session.events, session.record);
    return mock_mcu::run_transport_replay(session.events);
}

}  // namespace remotebsp::toolbusd
