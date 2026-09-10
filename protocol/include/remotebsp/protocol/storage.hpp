#pragma once

#include <cstdint>
#include <vector>

namespace remotebsp::protocol {

constexpr std::uint16_t kStorageProtocolVersion = 1;
constexpr std::uint32_t kMaximumStorageTransferBytes = 1024;
constexpr std::uint32_t kMaximumStorageTimeoutUs = 1000000;
constexpr std::uint16_t kStorageFlagEraseBeforeProgram = 0x0001;

struct StorageContract {
    std::uint16_t version{kStorageProtocolVersion};
    std::uint16_t flags{};
    std::uint32_t resource_id{};
    std::uint32_t capacity_bytes{};
    std::uint32_t erase_block_bytes{};
    std::uint32_t write_alignment_bytes{};
    std::uint32_t maximum_transfer_bytes{};
};

struct StorageRangeRequest {
    std::uint32_t resource_id{};
    std::uint32_t offset{};
    std::uint32_t length{};
    std::uint32_t timeout_us{};
};

struct StorageProgramRequest {
    std::uint32_t resource_id{};
    std::uint32_t offset{};
    std::uint32_t timeout_us{};
    std::vector<std::uint8_t> data;
};

struct StorageReadResult {
    std::uint32_t resource_id{};
    std::uint32_t offset{};
    std::vector<std::uint8_t> data;
};

std::vector<std::uint8_t> encode_storage_contract(const StorageContract& value);
StorageContract decode_storage_contract(const std::vector<std::uint8_t>& data);
std::vector<std::uint8_t> encode_storage_range_request(const StorageRangeRequest& value);
StorageRangeRequest decode_storage_range_request(const std::vector<std::uint8_t>& data);
std::vector<std::uint8_t> encode_storage_program_request(const StorageProgramRequest& value);
StorageProgramRequest decode_storage_program_request(const std::vector<std::uint8_t>& data);
std::vector<std::uint8_t> encode_storage_read_result(const StorageReadResult& value);
StorageReadResult decode_storage_read_result(const std::vector<std::uint8_t>& data);

}  // namespace remotebsp::protocol
