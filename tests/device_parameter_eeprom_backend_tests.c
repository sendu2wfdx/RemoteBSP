#include "remotebsp_embedded/device_parameter_eeprom.h"
#include <assert.h>
#include <string.h>

typedef struct { uint8_t bytes[1024]; bool fail_read, fail_write, fail_fill, corrupt; } io_t;
static bool rd(void* c,uint32_t o,uint8_t* d,size_t n){io_t*x=c;if(x->fail_read)return false;memcpy(d,x->bytes+o,n);return true;}
static bool wr(void* c,uint32_t o,const uint8_t*d,size_t n){io_t*x=c;if(x->fail_write){if(n)x->bytes[o]=d[0];return false;}memcpy(x->bytes+o,d,n);if(x->corrupt&&n)x->bytes[o]^=1;return true;}
static bool fill(void*c,uint32_t o,size_t n,uint8_t v){io_t*x=c;if(x->fail_fill)return false;memset(x->bytes+o,v,n);return true;}
int main(void){
 io_t io; uint8_t mirror[1024]; rbsp_device_parameter_eeprom_adapter a; rbsp_device_param_backend b;
 memset(&io,0xff,sizeof(io)); io.fail_read=io.fail_write=io.fail_fill=io.corrupt=false;
 rbsp_device_parameter_eeprom_config c={&io,1024,512,1,rd,wr,fill,mirror,sizeof(mirror)};
 assert(rbsp_device_parameter_eeprom_backend_init(&a,&b,&c)); assert(b.medium==RBSP_DEVICE_PARAM_MEDIUM_EXTERNAL_EEPROM);
 uint8_t v[2]={0x12,0x34}; assert(b.program(b.context,5,v,2)); assert(memcmp(b.map(b.context,5,2),v,2)==0);
 io.fail_write=true; v[0]=0x56; assert(!b.program(b.context,5,v,2)); assert(b.map(b.context,5,2)[0]==0x56); io.fail_write=false;
 io.corrupt=true; assert(!b.program(b.context,8,v,2)); io.corrupt=false;
 io.fail_fill=true; assert(!b.erase(b.context,0,512)); io.fail_fill=false; assert(b.erase(b.context,0,512));
 c.mirror_size--; assert(!rbsp_device_parameter_eeprom_backend_init(&a,&b,&c));
 return 0;
}
