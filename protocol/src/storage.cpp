#include "remotebsp/protocol/storage.hpp"

#include <limits>
#include <stdexcept>

namespace remotebsp::protocol {
namespace {
void u16(std::vector<std::uint8_t>& o, std::uint16_t v) { o.push_back(v); o.push_back(v >> 8U); }
void u32(std::vector<std::uint8_t>& o, std::uint32_t v) { for (unsigned i=0;i<4;++i)o.push_back(v>>(8U*i)); }
std::uint16_t r16(const std::vector<std::uint8_t>& d,std::size_t p){return static_cast<std::uint16_t>(d[p])|(static_cast<std::uint16_t>(d[p+1])<<8U);}
std::uint32_t r32(const std::vector<std::uint8_t>& d,std::size_t p){std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=static_cast<std::uint32_t>(d[p+i])<<(8U*i);return v;}
void range_valid(const StorageRangeRequest& v) {
    if (!v.resource_id || !v.length || v.length > kMaximumStorageTransferBytes ||
        !v.timeout_us || v.timeout_us > kMaximumStorageTimeoutUs ||
        v.offset > std::numeric_limits<std::uint32_t>::max() - v.length)
        throw std::invalid_argument("Storage 范围请求超出协议边界");
}
}
std::vector<std::uint8_t> encode_storage_contract(const StorageContract& v) {
    if(v.version!=kStorageProtocolVersion || (v.flags & ~kStorageFlagEraseBeforeProgram) || !v.resource_id ||
       !v.capacity_bytes || !v.erase_block_bytes || !v.write_alignment_bytes || !v.maximum_transfer_bytes ||
       v.maximum_transfer_bytes>kMaximumStorageTransferBytes || v.maximum_transfer_bytes<v.erase_block_bytes ||
       v.capacity_bytes%v.erase_block_bytes ||
       v.erase_block_bytes%v.write_alignment_bytes) throw std::invalid_argument("Storage 合同无效");
    std::vector<std::uint8_t> o; u16(o,v.version);u16(o,v.flags);u32(o,v.resource_id);u32(o,v.capacity_bytes);
    u32(o,v.erase_block_bytes);u32(o,v.write_alignment_bytes);u32(o,v.maximum_transfer_bytes);return o;
}
StorageContract decode_storage_contract(const std::vector<std::uint8_t>& d){if(d.size()!=24)throw std::invalid_argument("Storage 合同编码无效");StorageContract v{r16(d,0),r16(d,2),r32(d,4),r32(d,8),r32(d,12),r32(d,16),r32(d,20)};(void)encode_storage_contract(v);return v;}
std::vector<std::uint8_t> encode_storage_range_request(const StorageRangeRequest& v){range_valid(v);std::vector<std::uint8_t>o;u32(o,v.resource_id);u32(o,v.offset);u32(o,v.length);u32(o,v.timeout_us);return o;}
StorageRangeRequest decode_storage_range_request(const std::vector<std::uint8_t>& d){if(d.size()!=16)throw std::invalid_argument("Storage 范围请求编码无效");StorageRangeRequest v{r32(d,0),r32(d,4),r32(d,8),r32(d,12)};range_valid(v);return v;}
std::vector<std::uint8_t> encode_storage_program_request(const StorageProgramRequest& v){StorageRangeRequest range{v.resource_id,v.offset,static_cast<std::uint32_t>(v.data.size()),v.timeout_us};range_valid(range);std::vector<std::uint8_t>o;u32(o,v.resource_id);u32(o,v.offset);u32(o,v.timeout_us);u32(o,static_cast<std::uint32_t>(v.data.size()));o.insert(o.end(),v.data.begin(),v.data.end());return o;}
StorageProgramRequest decode_storage_program_request(const std::vector<std::uint8_t>& d){if(d.size()<17)throw std::invalid_argument("Storage 写请求编码无效");const auto n=r32(d,12);if(n>d.size()||d.size()!=16U+n)throw std::invalid_argument("Storage 写请求长度无效");StorageProgramRequest v{r32(d,0),r32(d,4),r32(d,8),{d.begin()+16,d.end()}};(void)encode_storage_program_request(v);return v;}
std::vector<std::uint8_t> encode_storage_read_result(const StorageReadResult& v){if(!v.resource_id||v.data.empty()||v.data.size()>kMaximumStorageTransferBytes)throw std::invalid_argument("Storage 读结果无效");std::vector<std::uint8_t>o;u32(o,v.resource_id);u32(o,v.offset);u32(o,static_cast<std::uint32_t>(v.data.size()));o.insert(o.end(),v.data.begin(),v.data.end());return o;}
StorageReadResult decode_storage_read_result(const std::vector<std::uint8_t>& d){if(d.size()<13)throw std::invalid_argument("Storage 读结果编码无效");const auto n=r32(d,8);if(!n||n>kMaximumStorageTransferBytes||d.size()!=12U+n)throw std::invalid_argument("Storage 读结果长度无效");StorageReadResult v{r32(d,0),r32(d,4),{d.begin()+12,d.end()}};if(!v.resource_id)throw std::invalid_argument("Storage 读结果资源无效");return v;}
}  // namespace remotebsp::protocol
