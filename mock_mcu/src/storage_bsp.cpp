#include "remotebsp/mock_mcu/storage_bsp.hpp"

#include <algorithm>
#include <stdexcept>

namespace remotebsp::mock_mcu {
protocol::StorageContract DeterministicStorageBsp::contract(std::uint32_t id) const {
    if (!id) throw std::invalid_argument("Storage 资源 ID 无效");
    return {protocol::kStorageProtocolVersion, protocol::kStorageFlagEraseBeforeProgram,
            id, 4096U, 256U, 4U, 256U};
}
std::vector<std::uint8_t>& DeterministicStorageBsp::bytes(std::uint32_t id) {
    auto [it, inserted] = contents_.try_emplace(id);
    if (inserted) it->second.assign(contract(id).capacity_bytes, 0xFFU);
    return it->second;
}
std::vector<std::uint8_t> DeterministicStorageBsp::read(const protocol::StorageRangeRequest& r) {
    auto& value=bytes(r.resource_id); return {value.begin()+r.offset,value.begin()+r.offset+r.length};
}
void DeterministicStorageBsp::erase(const protocol::StorageRangeRequest& r) {
    auto& value=bytes(r.resource_id);std::fill(value.begin()+r.offset,value.begin()+r.offset+r.length,0xFFU);
}
void DeterministicStorageBsp::program(const protocol::StorageProgramRequest& r) {
    auto& value=bytes(r.resource_id);
    for(std::size_t i=0;i<r.data.size();++i) if((value[r.offset+i] & r.data[i]) != r.data[i]) throw std::runtime_error("Storage 写入需要先擦除");
    for(std::size_t i=0;i<r.data.size();++i)value[r.offset+i]&=r.data[i];
}
}  // namespace remotebsp::mock_mcu
