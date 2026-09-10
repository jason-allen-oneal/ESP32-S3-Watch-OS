#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace nightglass::services {

// Additive extension. The separate capability characteristic prevents a new
// companion from sending these frames to older, otherwise compatible watches.
inline constexpr std::size_t kPremiumProfileBytes = 48;
inline constexpr std::size_t kPremiumContentBytes = 4096;
inline constexpr std::uint8_t kPremiumAlwaysOn = 1;
inline constexpr std::uint8_t kPremiumLargeText = 2;
inline constexpr std::uint8_t kPremiumReduceMotion = 4;

struct PremiumProfile {
    std::uint8_t face{1};
    std::uint8_t flags{0};
    std::uint32_t accent{0xa8ff32};
    // Battery, steps, weather, timer, alarm, notifications, distance, phone.
    std::array<std::uint8_t, 3> complications{1, 2, 0};
    std::array<std::uint8_t, 5> deck_order{0, 1, 2, 3, 4};
    std::uint8_t deck_mask{31};
    std::array<char, 25> name{"My Nightglass"};
    bool operator==(const PremiumProfile &) const = default;
};

bool valid_premium_profile(const PremiumProfile &profile);
bool decode_premium_profile(std::span<const std::uint8_t> frame,
                            PremiumProfile &profile, std::uint32_t &nonce);
std::array<std::uint8_t, kPremiumProfileBytes> encode_premium_profile(
    const PremiumProfile &profile, std::uint32_t nonce, bool response = false);
std::uint32_t premium_crc32(std::span<const std::uint8_t> bytes);

struct PremiumDocumentAction {
    std::uint8_t action{}, index{};
    std::array<char, 61> label{};
};
struct PremiumDocument {
    std::uint8_t kind{}, action_count{};
    std::array<char, 1601> body{};
    std::array<PremiumDocumentAction, 7> actions{};
    std::span<const std::uint8_t> artwork{}; // 40 x 40 RGB565, little endian.
};
bool decode_premium_document(std::span<const std::uint8_t> bytes, PremiumDocument &document);

enum class WatchSuggestionKind : std::uint8_t { none, timer, spotify, phone, inbox, ring };
struct WatchSuggestion {
    WatchSuggestionKind kind{WatchSuggestionKind::none};
    std::uint32_t seconds{};
    std::size_t text_length{};
};
// Suggestions are inert until a separate, physical confirmation in the UI.
WatchSuggestion parse_watch_suggestion(std::string_view text);

// One request-owned transaction, bounded and atomic: partial, duplicate,
// reordered, stale and wrong-target content can never become visible.
class PremiumTransfer {
public:
    void request(std::uint32_t token, std::uint8_t kind, std::uint32_t target);
    void clear();
    bool accept(std::span<const std::uint8_t> frame);
    [[nodiscard]] std::span<const std::uint8_t> content() const;
    std::uint32_t token() const { return token_; }
    std::uint32_t target() const { return target_; }
    std::uint8_t kind() const { return kind_; }
private:
    std::array<std::uint8_t, kPremiumContentBytes> data_{};
    std::uint32_t token_{0}, target_{0}, crc_{0};
    std::uint16_t length_{0}, received_{0};
    std::uint8_t kind_{0};
    bool begun_{false}, ready_{false};
};

// AOD uses black plus a small moving clock, never a frozen normal watch face.
constexpr int premium_aod_shift(std::uint32_t minute) {
    return (static_cast<int>(minute % 9) - 4) * 3;
}
constexpr bool premium_aod_allowed(bool enabled, bool battery_present, bool percent_valid,
                                    unsigned percent, std::int64_t age_us) {
    return enabled && battery_present && percent_valid && percent > 15 && percent <= 100 &&
           age_us >= 0 && age_us <= 30'000'000;
}

}  // namespace nightglass::services
