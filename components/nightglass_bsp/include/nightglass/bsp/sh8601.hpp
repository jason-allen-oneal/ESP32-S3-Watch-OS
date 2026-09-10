#pragma once

#include <cstdint>

namespace nightglass::bsp {

// The board configures 32-bit QSPI command words. Match the installed SH8601
// driver's tx_param: write-command opcode, then the DCS command in bits 15:8.
constexpr std::uint32_t sh8601_qspi_command(std::uint8_t command) {
    return (std::uint32_t{0x02} << 24) | (std::uint32_t{command} << 8);
}

}  // namespace nightglass::bsp
