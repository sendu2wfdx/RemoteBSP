#pragma once

#include "remotebsp/mock_mcu/transport_replay.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

namespace remotebsp::toolbusd {

constexpr std::size_t kLogicalLinkRecordingMaximumFileBytes = 16U * 1024U * 1024U;

struct LinkRecordingStatus {
    bool active{};
    std::string evidence_scope{"logical-link-boundary-only"};
    std::string output_name;
    std::size_t event_count{};
    std::size_t maximum_events{mock_mcu::kMaximumTransportReplayEvents};
    std::size_t maximum_file_bytes{kLogicalLinkRecordingMaximumFileBytes};
};

// 单进程受控录制器。目录由守护进程启动配置固定，调用者只能提供简单文件名。
class LinkRecordingController {
public:
    explicit LinkRecordingController(std::string fixed_directory);
    ~LinkRecordingController();

    std::unique_ptr<transport::LinkTransport> start(
        std::unique_ptr<transport::LinkTransport> inner,
        const std::string& output_name);
    // 运行期控制使用：先把链路永久包裹一次，之后 start/stop 只切换观察器，
    // 不替换正在被工作线程使用的 transport 对象。
    std::unique_ptr<transport::LinkTransport> wrap(
        std::unique_ptr<transport::LinkTransport> inner);
    void start(const std::string& output_name);
    std::string stop();
    LinkRecordingStatus status() const;

    static mock_mcu::TransportReplayRecord consume_offline(
        const std::string& fixed_directory, const std::string& output_name);

private:
    class SwitchTransport;
    void record_frame(mock_mcu::TransportReplayDirection,
                      std::uint32_t, const transport::LinkFrame&) noexcept;
    void record_send_failure(mock_mcu::TransportReplayDirection,
                             std::uint32_t,
                             const transport::LinkFrame&) noexcept;
    void record_receive_empty(mock_mcu::TransportReplayDirection,
                              std::uint32_t,
                              std::chrono::milliseconds) noexcept;
    void record_receive_failure(mock_mcu::TransportReplayDirection,
                                std::uint32_t,
                                std::chrono::milliseconds) noexcept;
    static std::string safe_path(const std::string&, const std::string&);
    std::string directory_;
    std::string output_name_;
    std::shared_ptr<mock_mcu::TransportSessionRecorder> recorder_;
    bool active_{};
    mutable std::mutex mutex_;
};

}  // namespace remotebsp::toolbusd
