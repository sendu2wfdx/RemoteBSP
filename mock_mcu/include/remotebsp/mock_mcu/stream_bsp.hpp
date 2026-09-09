#pragma once

#include "remotebsp/protocol/bus_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace remotebsp::mock_mcu {

enum class StreamPushStatus {
    Accepted,
    Backpressured,
    Failed,
};

class StreamBsp {
public:
    virtual ~StreamBsp() = default;
    virtual const protocol::StreamContract* contract(
        std::uint32_t resource_id) const noexcept = 0;
    virtual std::size_t buffered_bytes(
        std::uint32_t resource_id) const noexcept = 0;
    // 必须是整块原子操作；返回非 Accepted 时不得保留任何前缀。
    virtual StreamPushStatus push(
        std::uint32_t resource_id,
        const std::vector<std::uint8_t>& data) noexcept = 0;
    virtual void reset(std::uint32_t resource_id) noexcept = 0;
};

enum class MockStreamError {
    InvalidContract,
    DuplicateResource,
    ResourceNotFound,
};

class MockStreamException : public std::runtime_error {
public:
    MockStreamException(MockStreamError code, const char* message);
    MockStreamError code() const noexcept;

private:
    MockStreamError code_;
};

// 面向主机测试的有界 STREAM 接收端。它只保存资源合同与已接收字节，
// 不模拟 USB/Ethernet 的真实性能或时序保证。
class MockStreamBsp final : public StreamBsp {
public:
    void add_resource(const protocol::StreamContract& contract);
    std::vector<std::uint8_t> consume(std::uint32_t resource_id,
                                      std::size_t maximum_bytes);

    const protocol::StreamContract* contract(
        std::uint32_t resource_id) const noexcept override;
    std::size_t buffered_bytes(
        std::uint32_t resource_id) const noexcept override;
    StreamPushStatus push(
        std::uint32_t resource_id,
        const std::vector<std::uint8_t>& data) noexcept override;
    void reset(std::uint32_t resource_id) noexcept override;

private:
    struct Resource {
        protocol::StreamContract contract;
        std::vector<std::uint8_t> buffered;
    };
    std::unordered_map<std::uint32_t, Resource> resources_;
};

}  // namespace remotebsp::mock_mcu
