#pragma once

#include "remotebsp/mock_mcu/board_manifest.hpp"

#include <cstdint>
#include <string>

namespace remotebsp::mock_mcu {

// 将数字孪生的可观察状态原子写入 JSON 文件，供本地 GUI 只读展示。
void write_visual_state(const DigitalTwin& twin, std::uint64_t elapsed_ms,
                        const std::string& path);

}
