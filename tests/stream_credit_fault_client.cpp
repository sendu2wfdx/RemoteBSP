#include "remotebsp/client.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "用法: stream_credit_fault_client IPC套接字\n";
        return 2;
    }
    try {
        constexpr std::uint32_t resource_id = 251658241U;
        remotebsp::Client client(argv[1], 1U);
        bool online = false;
        for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
            try {
                const auto nodes = client.list_nodes();
                for (const auto& node : nodes) {
                    if (node.node_id == 1U && node.online && node.ready) {
                        online = true;
                        break;
                    }
                }
            } catch (const remotebsp::ClientException&) {
                // toolbusd 套接字创建与首次发现之间允许短暂不可用。
            }
            if (online) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        require(online, "Mock USB 节点未按期上线");
        static_cast<void>(client.acquire_resource(
            resource_id, 10000U,
            remotebsp::protocol::ResourceLeaseMode::SharedRead));
        const auto opened = client.stream_open({
            resource_id, 16U,
            static_cast<std::uint16_t>(
                remotebsp::protocol::kStreamFlagLossless |
                remotebsp::protocol::kStreamFlagCreditRequired),
            64U});

        bool response_lost = false;
        try {
            static_cast<void>(client.stream_read(opened.stream_id, 0U, 3000U));
        } catch (const std::exception&) {
            response_lost = true;
        }
        require(response_lost, "未命中信用已提交后的 IPC 响应丢失故障");

        // 必须使用同一个 Client 重试；其两阶段缓存应返回原数据块，而
        // toolbusd 的幂等窗口不能再次向远端发送 STREAM_CREDIT。
        const auto recovered = client.stream_read(opened.stream_id, 0U, 3000U);
        require(recovered.has_value(), "故障后未恢复原 STREAM 数据块");
        require(recovered->sequence == 0U, "恢复数据序号错误");
        require(std::string(recovered->data.begin(), recovered->data.end()) ==
                    "mock-stream\n",
                "恢复数据内容错误");
        const auto after_recovery = client.stream_status(opened.stream_id);
        require(after_recovery.available_credit_bytes == 64U,
                "信用重试导致窗口二次扩大");

        const auto next = client.stream_read(opened.stream_id, 1U, 3000U);
        require(next.has_value() && next->sequence == 1U,
                "恢复后的 STREAM 序号不连续");
        const auto after_next = client.stream_status(opened.stream_id);
        require(after_next.available_credit_bytes == 64U,
                "后续确认未维持精确信用窗口");

        client.stream_stop(opened.stream_id);
        bool stale_rejected = false;
        try {
            static_cast<void>(client.stream_read(opened.stream_id, 2U, 100U));
        } catch (const remotebsp::ClientException&) {
            stale_rejected = true;
        }
        require(stale_rejected, "停止后的旧 stream_id 未被拒绝");
        std::cout << "stream credit IPC fault recovery passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
