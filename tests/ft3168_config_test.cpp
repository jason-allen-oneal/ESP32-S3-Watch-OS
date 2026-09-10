#include "nightglass/bsp/ft3168.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct FakeIo {
    std::vector<std::string> calls;
    unsigned fail_at = 0;
    unsigned operations = 0;
    unsigned since_reset_release_ms = 0;
    bool reset_released = false;
    std::uint8_t id = nightglass::bsp::kFt3168DeviceId;
    std::uint8_t mode = nightglass::bsp::kFt3168ActiveMode;

    bool ok() { return ++operations != fail_at; }
    bool set_reset(bool asserted) {
        calls.push_back(asserted ? "reset asserted" : "reset released");
        reset_released = !asserted;
        since_reset_release_ms = 0;
        return ok();
    }
    void delay_ms(std::uint32_t milliseconds) {
        calls.push_back("delay " + std::to_string(milliseconds));
        if (reset_released) since_reset_release_ms += milliseconds;
    }
    bool read(std::uint8_t reg, std::uint8_t &value) {
        assert(reset_released && since_reset_release_ms >= 70);
        calls.push_back("read " + std::to_string(reg));
        assert(reg == 0xa0 || reg == 0xa5);
        value = reg == 0xa0 ? id : mode;
        return ok();
    }
    bool write(std::uint8_t reg, std::uint8_t value) {
        assert(reset_released && since_reset_release_ms >= 70);
        calls.push_back("write " + std::to_string(reg) + " " + std::to_string(value));
        assert(reg == 0xa5 && value == 0);  // Never generic FT5x06 tuning registers.
        return ok();
    }
};

int main() {
    using nightglass::bsp::configure_ft3168;
    std::uint8_t id = 0xff;
    std::uint8_t mode = 0xff;
    FakeIo valid;
    assert(configure_ft3168(valid, id, mode));
    assert(id == 3 && mode == 0);
    assert((valid.calls == std::vector<std::string>{
        "reset asserted", "delay 20", "reset released", "delay 100",
        "read 160", "write 165 0", "delay 20", "read 165"}));

    for (unsigned fail = 1; fail <= 5; ++fail) {
        FakeIo failing;
        failing.fail_at = fail;
        assert(!configure_ft3168(failing, id, mode));
        assert(failing.operations == fail);  // No further writes after a failure.
    }
    FakeIo wrong_chip;
    wrong_chip.id = 4;
    assert(!configure_ft3168(wrong_chip, id, mode));
    assert(wrong_chip.operations == 3);  // No mode write to an unexpected chip.

    FakeIo wrong_mode;
    wrong_mode.mode = 1;
    assert(!configure_ft3168(wrong_mode, id, mode));
    std::puts("Nightglass FT3168 configuration tests passed");
}
