#include "nightglass/services/rtc_sync.hpp"
#include <cassert>
#include <vector>
using namespace nightglass::services;
struct Io {
 int fail=0, calls=0; std::vector<unsigned> controls; std::array<std::uint8_t,8> data{};
 bool read_control(std::uint8_t &c) { c=5; return ++calls!=fail; }
 bool write_control(std::uint8_t c) { controls.push_back(c); return ++calls!=fail; }
 bool write_time(const std::array<std::uint8_t,8>&d) { data=d; return ++calls!=fail; }
};
int main() {
 CivilTime t{2024,2,29,4,23,59,58}; const auto epoch=civil_to_epoch(t);
 Io io; assert(sync_rtc(io,epoch)); assert((io.controls==std::vector<unsigned>{0x25,5}));
 assert((io.data==std::array<std::uint8_t,8>{4,0x58,0x59,0x23,0x29,4,2,0x24}));
 for(int fail=1;fail<=4;++fail) { Io broken; broken.fail=fail; assert(!sync_rtc(broken,epoch)); if(fail>=2) assert(broken.controls.back()==5); }
 Io invalid; assert(!sync_rtc(invalid,0)); assert(invalid.calls==0);
 std::array<std::uint8_t,12> frame{'N','G','T','1'};
 for(unsigned i=0;i<8;++i) frame[4+i]=static_cast<std::uint64_t>(epoch)>>(8*i);
 assert(decode_rtc_sync(frame)==epoch); assert(decode_rtc_sync(std::span(frame).first(11))==0);
 frame.fill(255); assert(decode_rtc_sync(frame)==0);
 assert(!valid_rtc_epoch(4102444800LL));
}
