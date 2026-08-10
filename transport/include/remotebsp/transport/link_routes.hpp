#pragma once

#include <cstdint>

namespace remotebsp::transport {

/*
 * 这些数值原先直接作为 CAN ID 使用。现在统一解释为逻辑路由号，
 * CAN 保持原有线上的 ID，USB 则在自己的帧头中携带相同路由号。
 */
constexpr std::uint32_t kDiscoveryRoute = 0x700U;
constexpr std::uint32_t kNodeRequestBaseRoute = 0x600U;
constexpr std::uint32_t kNodeResponseBaseRoute = 0x580U;
constexpr std::uint32_t kNodeEventBaseRoute = 0x500U;
constexpr std::uint32_t kProvisionalResponseBaseRoute = 0x480U;
constexpr std::uint32_t kMaximumNodeId = 127U;

}
