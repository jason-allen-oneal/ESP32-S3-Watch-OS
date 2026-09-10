#include "nightglass/services/premium_protocol.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace nightglass::services;
namespace {
void put32(std::uint8_t *p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = value >> (8 * i);
}
void profile_tests() {
    PremiumProfile profile{}, decoded{};
    assert(valid_premium_profile(profile));
    const auto bytes = encode_premium_profile(profile, 0x12345678);
    std::string hex;
    for (auto byte : bytes) { char value[3]{}; std::snprintf(value, sizeof(value), "%02x", byte); hex += value; }
    assert(hex == "0170785634120100a8ff3201020000010203041f0d4d79204e69676874676c6173730000000000000000000000000000");
    std::uint32_t nonce{};
    assert(decode_premium_profile(bytes, decoded, nonce) && nonce == 0x12345678 && decoded == profile);
    for (std::size_t length = 0; length < bytes.size(); ++length)
        assert(!decode_premium_profile(std::span(bytes.data(), length), decoded, nonce));
    for (unsigned face = 0; face < 3; ++face) for (unsigned flags = 0; flags < 8; ++flags) {
        profile.face = face; profile.flags = flags;
        assert(decode_premium_profile(encode_premium_profile(profile, 99), decoded, nonce));
        assert(decoded == profile);
    }
    profile = {}; profile.deck_order[0] = 1; assert(!valid_premium_profile(profile));
    profile = {}; profile.deck_mask = 0; assert(!valid_premium_profile(profile));
    profile = {}; profile.accent = 0x101010; assert(!valid_premium_profile(profile));
    profile = {}; profile.accent = 0x1000000; assert(!valid_premium_profile(profile));
    profile = {}; profile.complications[1] = 8; assert(!valid_premium_profile(profile));
    profile = {}; profile.name.fill('a'); assert(!valid_premium_profile(profile));
    profile = {}; profile.name[0] = '\n'; assert(!valid_premium_profile(profile));
    auto bad = bytes; bad[47] = 1; assert(!decode_premium_profile(bad, decoded, nonce));
    bad = bytes; bad[21] = 0; assert(!decode_premium_profile(bad, decoded, nonce));
    bad = bytes; bad[23] = 0; assert(!decode_premium_profile(bad, decoded, nonce));
    bad = bytes; bad[20] = 25; assert(!decode_premium_profile(bad, decoded, nonce));
}
void transfer_tests() {
    const std::vector<std::uint8_t> document{1, 2, 1, 0, 5, 0, 'H', 'e', 'l', 'l', 'o', 6, 0, 4, 'O', 'p', 'e', 'n'};
    PremiumDocument decoded{};
    assert(decode_premium_document(document, decoded));
    assert(std::strcmp(decoded.body.data(), "Hello") == 0 && decoded.action_count == 1 && decoded.actions[0].action == 6);
    auto bad = document; bad[11] = 1; assert(!decode_premium_document(bad, decoded));
    bad = document; bad[6] = 0xff; assert(!decode_premium_document(bad, decoded));
    bad = document; bad.push_back(0); assert(!decode_premium_document(bad, decoded));
    for (std::size_t length = 0; length < document.size(); ++length)
        assert(!decode_premium_document(std::span(document.data(), length), decoded));
    PremiumTransfer transfer;
    transfer.request(100, 2, 200);
    std::array<std::uint8_t, 13> begin{1, 0x73};
    put32(begin.data() + 2, 100); begin[6] = 2; put32(begin.data() + 7, 200); begin[11] = document.size();
    auto wrong = begin; wrong[7] = 201;
    assert(!transfer.accept(wrong));
    assert(transfer.accept(begin));
    assert(!transfer.accept(begin));
    std::vector<std::uint8_t> data(8 + document.size(), 0); data[0] = 1; data[1] = 0x74;
    put32(data.data() + 2, 100); std::copy(document.begin(), document.end(), data.begin() + 8);
    bad = data; bad[6] = 1; assert(!transfer.accept(bad));
    std::array<std::uint8_t, 10> end{1, 0x75}; put32(end.data() + 2, 100);
    put32(end.data() + 6, premium_crc32(document));
    assert(!transfer.accept(end) && transfer.content().empty());
    assert(transfer.accept(data));
    assert(!transfer.accept(data) && transfer.content().empty());
    assert(transfer.accept(end) && transfer.content().size() == document.size());
    assert(!transfer.accept(end));
    transfer.request(101, 2, 200);
    assert(transfer.content().empty() && !transfer.accept(data) && !transfer.accept(end));
    transfer.request(100, 2, 200);
    assert(transfer.accept(begin) && transfer.accept(data));
    end[6] ^= 1; assert(!transfer.accept(end) && transfer.content().empty() && transfer.token() == 0);
    transfer.request(1, 2, 0); assert(transfer.token() == 0);
    std::vector<std::uint8_t> art(3207, 0); art[0] = 1; art[1] = 1; art[3] = 1; art[4] = 1; art[6] = 'A';
    assert(decode_premium_document(art, decoded) && decoded.artwork.size() == 3200);
    art.pop_back(); assert(!decode_premium_document(art, decoded));
}
void suggestion_tests() {
    for (const auto seconds : {60, 300, 3600}) {
        const auto parsed = parse_watch_suggestion("Review this timer.\n[WATCH:TIMER:" + std::to_string(seconds) + "]");
        assert(parsed.kind == WatchSuggestionKind::timer && parsed.seconds == static_cast<unsigned>(seconds));
        assert(parsed.text_length == std::strlen("Review this timer."));
    }
    for (const auto *invalid : {"[WATCH:TIMER:300]", "OK\n[WATCH:TIMER:0]", "OK\n[WATCH:TIMER:59]",
         "OK\n[WATCH:TIMER:3601]", "OK\n[WATCH:TIMER:99999999999]", "OK\n[WATCH:TIMER:-300]",
         "OK\n[WATCH:TIMER:060]", "OK\n[WATCH:TIMER:300]\nmore", "OK\n[WATCH:EXEC:ls]",
         "[WATCH:OPEN:PHONE]\n[WATCH:OPEN:PHONE]", "OK\n[WATCH:OPEN:https://example.com]"})
        assert(parse_watch_suggestion(invalid).kind == WatchSuggestionKind::none);
    assert(parse_watch_suggestion("OK\n[WATCH:OPEN:SPOTIFY]").kind == WatchSuggestionKind::spotify);
    assert(parse_watch_suggestion("OK\n[WATCH:PHONE:RING]").kind == WatchSuggestionKind::ring);
    for (unsigned minute = 0; minute < 1000; ++minute) {
        const auto shift = premium_aod_shift(minute);
        assert(shift >= -12 && shift <= 12);
        assert(shift != premium_aod_shift(minute + 1));
    }
    assert(premium_aod_allowed(true, true, true, 16, 30'000'000));
    assert(!premium_aod_allowed(true, true, true, 15, 0));
    assert(!premium_aod_allowed(false, true, true, 80, 0));
    assert(!premium_aod_allowed(true, false, true, 80, 0));
    assert(!premium_aod_allowed(true, true, false, 80, 0));
    assert(!premium_aod_allowed(true, true, true, 80, 30'000'001));
    assert(!premium_aod_allowed(true, true, true, 80, -1));
    assert(!premium_aod_allowed(true, true, true, 101, 0));
}
}
int main() {
    profile_tests(); transfer_tests(); suggestion_tests();
    std::puts("Nightglass premium profile, content ownership, AOD shift and suggestion tests passed");
}
