#pragma once
#include <array>
#include <span>
#include <cstdint>
#include "nightglass/services/time_math.hpp"
namespace nightglass::services {
inline bool valid_rtc_epoch(std::int64_t epoch) { return epoch >= 1577836800LL && epoch < 4102444800LL; }
inline bool is_rtc_sync_frame(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 4 && bytes[0]=='N' && bytes[1]=='G' && bytes[2]=='T' && bytes[3]=='1';
}
inline std::int64_t decode_rtc_sync(std::span<const std::uint8_t> bytes) {
    if (bytes.size()!=12 || !is_rtc_sync_frame(bytes)) return 0;
    std::uint64_t epoch=0;
    for (unsigned i=0;i<8;++i) epoch |= std::uint64_t(bytes[4+i]) << (8*i);
    return epoch>=1577836800ULL && epoch<4102444800ULL ? static_cast<std::int64_t>(epoch) : 0;
}
template<class Io> bool sync_rtc(Io &io, std::int64_t epoch) {
    if (!valid_rtc_epoch(epoch)) return false;
    const auto t=epoch_to_civil(epoch);
    if (!valid_civil(t)) return false;
    std::uint8_t control=0;
    if (!io.read_control(control)) return false;
    // Preserve capacitance/interrupt configuration, never assert software reset.
    const auto running=static_cast<std::uint8_t>(control & ~0x32U);
    if (!io.write_control(static_cast<std::uint8_t>(running | 0x20U))) {
        io.write_control(running);
        return false;
    }
    const auto bcd=[](unsigned n) { return static_cast<std::uint8_t>((n/10)*16+n%10); };
    const std::array<std::uint8_t,8> data{0x04,bcd(t.second),bcd(t.minute),bcd(t.hour),bcd(t.day),t.weekday,bcd(t.month),bcd(t.year-2000)};
    const bool written=io.write_time(data);
    const bool restarted=io.write_control(running);
    return written && restarted;
}
}
