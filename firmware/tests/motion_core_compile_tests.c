#include "remotebsp_embedded/core.h"

/*
 * 该目标使用启用运动模块的配置同时编译Remote Core和定时执行器。
 * 协议行为由主机/Mock端到端测试覆盖；这里专门防止可选固件组合发生编译回归。
 */
int main(void) {
    rbsp_core_t core;
    (void)core;
    return 0;
}
