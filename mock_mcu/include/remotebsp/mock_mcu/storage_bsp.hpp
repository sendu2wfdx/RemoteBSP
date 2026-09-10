#pragma once

#include "remotebsp/protocol/storage.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace remotebsp::mock_mcu {

class StorageBsp {
public:
    virtual ~StorageBsp() = default;
    virtual protocol::StorageContract contract(std::uint32_t resource_id) const = 0;
    virtual std::vector<std::uint8_t> read(const protocol::StorageRangeRequest& request) = 0;
    virtual void erase(const protocol::StorageRangeRequest& request) = 0;
    virtual void program(const protocol::StorageProgramRequest& request) = 0;
};

class DeterministicStorageBsp final : public StorageBsp {
public:
    protocol::StorageContract contract(std::uint32_t resource_id) const override;
    std::vector<std::uint8_t> read(const protocol::StorageRangeRequest& request) override;
    void erase(const protocol::StorageRangeRequest& request) override;
    void program(const protocol::StorageProgramRequest& request) override;

private:
    std::vector<std::uint8_t>& bytes(std::uint32_t resource_id);
    std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> contents_;
};

}  // namespace remotebsp::mock_mcu
