#include "remotebsp/mock_mcu/stream_bsp.hpp"

#include <algorithm>
#include <cstddef>

namespace remotebsp::mock_mcu {

MockStreamException::MockStreamException(MockStreamError code,
                                         const char* message)
    : std::runtime_error(message), code_(code) {}

MockStreamError MockStreamException::code() const noexcept { return code_; }

void MockStreamBsp::add_resource(
    const protocol::StreamContract& contract) {
    static_cast<void>(protocol::encode_stream_contract(contract));
    if (contract.direction != protocol::StreamDirection::HostToNode ||
        (contract.flags & protocol::kStreamFlagTimestamped) != 0U) {
        throw MockStreamException(
            MockStreamError::InvalidContract,
            "当前 Mock 流后端只实现无时间戳的主机到节点字节流");
    }
    const auto inserted = resources_.emplace(
        contract.resource_id, Resource{contract, {}});
    if (!inserted.second) {
        throw MockStreamException(MockStreamError::DuplicateResource,
                                  "Mock 流资源 ID 重复");
    }
}

std::vector<std::uint8_t> MockStreamBsp::consume(
    std::uint32_t resource_id, std::size_t maximum_bytes) {
    const auto found = resources_.find(resource_id);
    if (found == resources_.end()) {
        throw MockStreamException(MockStreamError::ResourceNotFound,
                                  "Mock 流资源不存在");
    }
    auto& buffered = found->second.buffered;
    const auto count = std::min(maximum_bytes, buffered.size());
    std::vector<std::uint8_t> output(buffered.begin(),
                                     buffered.begin() +
                                         static_cast<std::ptrdiff_t>(count));
    buffered.erase(buffered.begin(),
                   buffered.begin() + static_cast<std::ptrdiff_t>(count));
    return output;
}

const protocol::StreamContract* MockStreamBsp::contract(
    std::uint32_t resource_id) const noexcept {
    const auto found = resources_.find(resource_id);
    return found == resources_.end() ? nullptr : &found->second.contract;
}

std::size_t MockStreamBsp::buffered_bytes(
    std::uint32_t resource_id) const noexcept {
    const auto found = resources_.find(resource_id);
    return found == resources_.end() ? 0U : found->second.buffered.size();
}

StreamPushStatus MockStreamBsp::push(
    std::uint32_t resource_id,
    const std::vector<std::uint8_t>& data) noexcept {
    const auto found = resources_.find(resource_id);
    if (found == resources_.end()) {
        return StreamPushStatus::Failed;
    }
    auto& resource = found->second;
    if (resource.buffered.size() > resource.contract.buffer_capacity_bytes ||
        data.size() > resource.contract.buffer_capacity_bytes -
                          resource.buffered.size()) {
        return StreamPushStatus::Backpressured;
    }
    try {
        resource.buffered.insert(resource.buffered.end(), data.begin(),
                                 data.end());
    } catch (...) {
        return StreamPushStatus::Failed;
    }
    return StreamPushStatus::Accepted;
}

void MockStreamBsp::reset(std::uint32_t resource_id) noexcept {
    const auto found = resources_.find(resource_id);
    if (found != resources_.end()) {
        found->second.buffered.clear();
    }
}

}  // namespace remotebsp::mock_mcu
