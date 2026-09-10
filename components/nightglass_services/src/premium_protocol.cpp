#include "nightglass/services/premium_protocol.hpp"

#include <algorithm>
#include <cstring>

namespace nightglass::services {
namespace {
std::uint32_t u32(const std::uint8_t *p) {
    return p[0] | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) |
           (std::uint32_t{p[3]} << 24);
}
void put32(std::uint8_t *p, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = n >> (8 * i);
}
}

bool valid_premium_profile(const PremiumProfile &p) {
    if (p.face > 2 || p.flags > 7 || p.accent > 0xffffff ||
        p.deck_mask == 0 || p.deck_mask > 31) return false;
    // Accent is used for text on black: reject imported invisible palettes.
    if (((p.accent >> 16) * 3 + ((p.accent >> 8) & 255) * 6 +
         (p.accent & 255)) < 960) return false;
    unsigned seen = 0;
    for (auto card : p.deck_order) {
        if (card > 4 || (seen & (1U << card))) return false;
        seen |= 1U << card;
    }
    for (auto field : p.complications) if (field > 7) return false;
    const auto length = strnlen(p.name.data(), p.name.size());
    if (length == 0 || length > 24) return false;
    if (std::all_of(p.name.begin(), p.name.begin() + length, [](char c) { return c == ' '; })) return false;
    return std::all_of(p.name.begin(), p.name.begin() + length,
                       [](unsigned char c) { return c >= 32 && c <= 126; });
}

bool decode_premium_profile(std::span<const std::uint8_t> f,
                            PremiumProfile &profile, std::uint32_t &nonce) {
    if (f.size() != kPremiumProfileBytes || f[0] != 1 ||
        (f[1] != 0x70 && f[1] != 0x71) || f[20] == 0 || f[20] > 24) return false;
    for (std::size_t i = 21 + f[20]; i < f.size(); ++i) if (f[i]) return false;
    if (!std::all_of(f.begin() + 21, f.begin() + 21 + f[20],
        [](std::uint8_t c) { return c >= 32 && c <= 126; })) return false;
    PremiumProfile p{};
    p.face = f[6]; p.flags = f[7];
    p.accent = (std::uint32_t{f[8]} << 16) | (std::uint32_t{f[9]} << 8) | f[10];
    std::copy_n(f.begin() + 11, 3, p.complications.begin());
    std::copy_n(f.begin() + 14, 5, p.deck_order.begin());
    p.deck_mask = f[19];
    p.name.fill(0);
    std::copy_n(f.begin() + 21, f[20], p.name.begin());
    if (!valid_premium_profile(p)) return false;
    profile = p; nonce = u32(f.data() + 2);
    return true;
}

std::array<std::uint8_t, kPremiumProfileBytes> encode_premium_profile(
    const PremiumProfile &p, std::uint32_t nonce, bool response) {
    std::array<std::uint8_t, kPremiumProfileBytes> f{};
    if (!valid_premium_profile(p)) return f;
    f[0] = 1; f[1] = response ? 0x71 : 0x70;
    put32(f.data() + 2, nonce);
    f[6] = p.face; f[7] = p.flags;
    f[8] = p.accent >> 16; f[9] = p.accent >> 8; f[10] = p.accent;
    std::copy(p.complications.begin(), p.complications.end(), f.begin() + 11);
    std::copy(p.deck_order.begin(), p.deck_order.end(), f.begin() + 14);
    f[19] = p.deck_mask;
    f[20] = strnlen(p.name.data(), 24);
    std::copy_n(p.name.begin(), f[20], f.begin() + 21);
    return f;
}

std::uint32_t premium_crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffff;
    for (auto byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return ~crc;
}

bool decode_premium_document(std::span<const std::uint8_t> f, PremiumDocument &out) {
    out = {};
    if (f.size() < 6 || f.size() > kPremiumContentBytes || f[0] != 1 ||
        f[1] < 1 || f[1] > 2 || f[2] > 7 || f[3] > 1 || (f[1] == 2 && f[3])) return false;
    const std::size_t length = f[4] | (unsigned{f[5]} << 8);
    if (!length || length > 1600 || length + 6 > f.size()) return false;
    auto printable = [](std::uint8_t c) { return c == '\n' || (c >= 32 && c <= 126); };
    if (!std::all_of(f.begin() + 6, f.begin() + 6 + length, printable)) return false;
    std::copy_n(f.begin() + 6, length, out.body.begin());
    out.kind = f[1]; out.action_count = f[2];
    std::size_t cursor = 6 + length;
    for (unsigned i = 0; i < out.action_count; ++i) {
        if (cursor + 3 > f.size()) return false;
        auto &a = out.actions[i]; a.action = f[cursor++]; a.index = f[cursor++];
        const auto size = f[cursor++];
        const bool valid_action = out.kind == 1
            ? (a.action == 1 && a.index < 4) || (a.action >= 2 && a.action <= 4 && !a.index)
            : (a.action == 5 && a.index < 4) || (a.action == 6 && !a.index);
        if (!valid_action || !size || size > 60 || cursor + size > f.size()) return false;
        if (!std::all_of(f.begin() + cursor, f.begin() + cursor + size,
                         [](std::uint8_t c) { return c >= 32 && c <= 126; })) return false;
        std::copy_n(f.begin() + cursor, size, a.label.begin()); cursor += size;
    }
    const std::size_t art_size = f[3] ? 3200 : 0;
    if (cursor + art_size != f.size()) return false;
    out.artwork = f.subspan(cursor, art_size);
    return true;
}

WatchSuggestion parse_watch_suggestion(std::string_view text) {
    WatchSuggestion result{}; result.text_length = text.size();
    if (text.size() > 2048) return result;
    const auto start = text.rfind("\n[WATCH:");
    if (start == std::string_view::npos || text.find("[WATCH:") != start + 1) return result;
    auto marker = text.substr(start + 8);
    if (marker.empty() || marker.back() != ']') return result;
    marker.remove_suffix(1);
    if (marker == "OPEN:SPOTIFY") result.kind = WatchSuggestionKind::spotify;
    else if (marker == "OPEN:PHONE") result.kind = WatchSuggestionKind::phone;
    else if (marker == "OPEN:INBOX") result.kind = WatchSuggestionKind::inbox;
    else if (marker == "PHONE:RING") result.kind = WatchSuggestionKind::ring;
    else if (marker.starts_with("TIMER:")) {
        marker.remove_prefix(6);
        if (marker.empty() || marker.size() > 4 || marker.front() == '0') return result;
        std::uint32_t seconds = 0;
        for (char c : marker) {
            if (c < '0' || c > '9') return result;
            seconds = seconds * 10 + static_cast<unsigned>(c - '0');
        }
        if (seconds < 60 || seconds > 3600) return result;
        result.kind = WatchSuggestionKind::timer; result.seconds = seconds;
    }
    if (result.kind != WatchSuggestionKind::none) result.text_length = start;
    return result;
}

void PremiumTransfer::clear() {
    data_.fill(0); token_ = target_ = crc_ = 0;
    length_ = received_ = 0; kind_ = 0; begun_ = ready_ = false;
}
void PremiumTransfer::request(std::uint32_t token, std::uint8_t kind, std::uint32_t target) {
    clear();
    if (token && kind >= 1 && kind <= 2 && (kind == 1 || target)) {
        token_ = token; kind_ = kind; target_ = target;
    }
}
bool PremiumTransfer::accept(std::span<const std::uint8_t> f) {
    if (f.size() < 6 || f[0] != 1 || !token_ || u32(f.data() + 2) != token_ || ready_)
        return false;
    if (f[1] == 0x73) {
        if (f.size() != 13 || begun_ || f[6] != kind_ || u32(f.data() + 7) != target_)
            return false;
        const unsigned length = f[11] | (unsigned{f[12]} << 8);
        if (!length || length > data_.size()) return false;
        length_ = length; begun_ = true;
        return true;
    }
    if (!begun_) return false;
    if (f[1] == 0x74) {
        if (f.size() < 9 || f.size() > 244) return false;
        const unsigned offset = f[6] | (unsigned{f[7]} << 8);
        if (offset != received_ || f.size() - 8 > static_cast<std::size_t>(length_ - received_)) return false;
        std::copy(f.begin() + 8, f.end(), data_.begin() + received_);
        received_ += f.size() - 8;
        return true;
    }
    if (f[1] != 0x75 || f.size() != 10 || received_ != length_) return false;
    crc_ = u32(f.data() + 6);
    if (crc_ != premium_crc32(std::span(data_.data(), length_))) { clear(); return false; }
    ready_ = true;
    return true;
}
std::span<const std::uint8_t> PremiumTransfer::content() const {
    return ready_ ? std::span(data_.data(), length_) : std::span<const std::uint8_t>{};
}
}  // namespace nightglass::services
