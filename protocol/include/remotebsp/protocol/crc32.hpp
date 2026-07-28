#pragma once

#include <cstddef>
#include <cstdint>

namespace remotebsp::protocol {

// IEEE 802.3 CRC-32，多项式为 0xEDB88320。
std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept;

}
