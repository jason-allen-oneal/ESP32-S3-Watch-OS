#include <cassert>

#include "nightglass/services/time_math.hpp"

using nightglass::services::CivilTime;
using nightglass::services::apply_utc_offset;
using nightglass::services::civil_to_epoch;
using nightglass::services::epoch_to_civil;
using nightglass::services::valid_civil;

int main() {
    const CivilTime leap{2024, 2, 29, 4, 23, 59, 58};
    assert(valid_civil(leap));
    const auto epoch = civil_to_epoch(leap);
    const auto round_trip = epoch_to_civil(epoch);
    assert(round_trip.year == 2024 && round_trip.month == 2 && round_trip.day == 29);
    assert(round_trip.hour == 23 && round_trip.minute == 59 && round_trip.second == 58);

    const auto rolled = apply_utc_offset(leap, 120);
    assert(rolled.year == 2024 && rolled.month == 3 && rolled.day == 1);
    assert(rolled.hour == 1 && rolled.minute == 59);

    const CivilTime new_year{2025, 1, 1, 3, 0, 15, 0};
    const auto western = apply_utc_offset(new_year, -300);
    assert(western.year == 2024 && western.month == 12 && western.day == 31);
    assert(western.hour == 19 && western.minute == 15);

    assert(!valid_civil(CivilTime{2025, 2, 29, 0, 0, 0, 0}));
    assert(valid_civil(CivilTime{2100, 2, 28, 0, 0, 0, 0}));
    return 0;
}
