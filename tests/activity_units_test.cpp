#include <cassert>
#include <cstring>

#include "nightglass/services/activity_units.hpp"

using namespace nightglass::services;

int main() {
    char text[32]{};
    format_activity_distance(text, sizeof(text), 1'132'500, ActivityUnits::metric);
    assert(std::strcmp(text, "1.13 km") == 0);
    format_activity_distance(text, sizeof(text), 1'132'500, ActivityUnits::imperial);
    assert(std::strcmp(text, "0.70 mi") == 0);
    format_activity_distance(text, sizeof(text), 30'480, ActivityUnits::imperial);
    assert(std::strcmp(text, "100 ft") == 0);
    format_activity_distance(text, sizeof(text), 42'000, ActivityUnits::metric);
    assert(std::strcmp(text, "42 m") == 0);
    format_step_length(text, sizeof(text), 750, ActivityUnits::imperial);
    assert(std::strcmp(text, "STEP LENGTH  29.5 in") == 0);
    format_step_length(text, sizeof(text), 750, ActivityUnits::metric);
    assert(std::strcmp(text, "STEP LENGTH  0.75 m") == 0);
    assert(next_step_length(750, ActivityUnits::metric) == 800);
    assert(next_step_length(750, ActivityUnits::imperial) == 762);
    assert(next_step_length(762, ActivityUnits::imperial) == 775);
    assert(next_step_length(1500, ActivityUnits::metric) == 300);
    assert(next_step_length(1499, ActivityUnits::imperial) == 305);
}
