#include "remotebsp/mock_mcu/remote_core.hpp"
#include "remotebsp/protocol/resource.hpp"
#include "remotebsp/protocol/storage.hpp"

#include <cassert>
#include <memory>
#include <stdexcept>

using namespace remotebsp;
namespace {
protocol::Packet request(protocol::Command command,std::vector<std::uint8_t> payload={},std::uint32_t session=7){
    protocol::Packet p;p.header.message_type=protocol::MessageType::Request;p.header.command=static_cast<std::uint16_t>(command);p.header.session_id=session;p.payload=std::move(payload);return p;
}
mock_mcu::StatusCode status(const protocol::Packet& p){return static_cast<mock_mcu::StatusCode>(p.payload.at(0));}
std::vector<std::uint8_t> body(const protocol::Packet& p){assert(status(p)==mock_mcu::StatusCode::Ok);return {p.payload.begin()+1,p.payload.end()};}
class SelectiveStorage final : public mock_mcu::StorageBsp {
public:
 protocol::StorageContract contract(std::uint32_t id)const override{return {1,1,id,4096,256,4,256};}
 std::vector<std::uint8_t> read(const protocol::StorageRangeRequest& r)override{if(r.resource_id==bad)throw std::runtime_error("故障");return std::vector<std::uint8_t>(r.length,0x5A);}
 void erase(const protocol::StorageRangeRequest& r)override{if(r.resource_id==bad)throw std::runtime_error("故障");}
 void program(const protocol::StorageProgramRequest& r)override{if(r.resource_id==bad)throw std::runtime_error("故障");}
 std::uint32_t bad{0x08000001};
};
}
int main(){
 const std::uint32_t a=0x08000001,b=0x08000002;
 const std::uint32_t access=protocol::kResourceAccessLeaseSupported|protocol::kResourceAccessLeaseRequired|protocol::kResourceAccessSharedRead;
 std::vector<protocol::ResourceDescriptor> resources{{a,protocol::ResourceType::Storage,1,protocol::kResourceFlagNative,0,0},{b,protocol::ResourceType::Storage,2,protocol::kResourceFlagNative,0,0}};
 std::vector<protocol::ResourceContract> contracts{{a,1,static_cast<std::uint16_t>(access),0,0,0,0,0,0},{b,1,static_cast<std::uint16_t>(access),0,0,0,0,0,0}};
 mock_mcu::RemoteCore core({},mock_mcu::capability_mask(mock_mcu::Capability::Storage),nullptr,nullptr,resources,contracts);
 auto contract=protocol::decode_storage_contract(body(core.handle(request(protocol::Command::StorageContract,protocol::encode_resource_id(a)))));
 assert(contract.capacity_bytes==4096 && contract.erase_block_bytes==256 && contract.write_alignment_bytes==4);
 assert(status(core.handle(request(protocol::Command::StorageRead,protocol::encode_storage_range_request({a,0,4,1000}))))==mock_mcu::StatusCode::AccessDenied);
 body(core.handle(request(protocol::Command::ResourceAcquire,protocol::encode_resource_lease_request({a,1000,protocol::ResourceLeaseMode::Exclusive}))));
 body(core.handle(request(protocol::Command::StorageErase,protocol::encode_storage_range_request({a,0,256,1000}))));
 body(core.handle(request(protocol::Command::StorageProgram,protocol::encode_storage_program_request({a,0,1000,{0xF0,0x0F,0xAA,0x55}}))));
 auto read=protocol::decode_storage_read_result(body(core.handle(request(protocol::Command::StorageRead,protocol::encode_storage_range_request({a,0,4,1000})))));
 assert((read.data==std::vector<std::uint8_t>{0xF0,0x0F,0xAA,0x55}));
 assert(status(core.handle(request(protocol::Command::StorageErase,protocol::encode_storage_range_request({a,1,256,1000}))))==mock_mcu::StatusCode::InvalidPayload);
 assert(status(core.handle(request(protocol::Command::StorageProgram,protocol::encode_storage_program_request({a,4092,1000,{1,2,3,4,5,6,7,8}}))))==mock_mcu::StatusCode::InvalidPayload);
 core.set_storage_bsp(std::make_shared<SelectiveStorage>());
 assert(status(core.handle(request(protocol::Command::StorageRead,protocol::encode_storage_range_request({a,0,4,1000}))))==mock_mcu::StatusCode::ResourceFailed);
 body(core.handle(request(protocol::Command::ResourceAcquire,protocol::encode_resource_lease_request({b,1000,protocol::ResourceLeaseMode::SharedRead}))));
    auto healthy=protocol::decode_storage_read_result(body(core.handle(request(protocol::Command::StorageRead,protocol::encode_storage_range_request({b,0,4,1000})))));
    assert(healthy.data==std::vector<std::uint8_t>(4,0x5A));
 assert(status(core.handle(request(protocol::Command::StorageProgram,
     protocol::encode_storage_program_request({b,0,1000,{0,0,0,0}}))))==mock_mcu::StatusCode::AccessDenied);
 bool rejected=false;try{(void)protocol::encode_storage_range_request({a,0,1025,1000});}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
 return 0;
}
