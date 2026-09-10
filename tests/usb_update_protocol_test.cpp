#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "nightglass/services/usb_update_protocol.hpp"

using namespace nightglass::services;

int main() {
    const std::array<std::uint8_t, 10> status_query{
        1, kUpdateStatusQueryOpcode, 1, 2, 3, 4, 5, 6, 7, 8};
    const auto encoded = encode_usb_update_envelope(status_query);
    assert(encoded.size == status_query.size() + kUsbUpdateEnvelopeHeaderSize +
                               kUsbUpdateEnvelopeChecksumSize);
    assert(encoded.bytes[0] == 'N' && encoded.bytes[3] == '1');
    assert(encoded.bytes[4] == status_query.size() && encoded.bytes[5] == 0);
    assert(usb_update_crc32({}) == 0);
    assert(usb_update_crc32(status_query) == 0x257dc53aU);

    UsbUpdateEnvelopeDecoder decoder{};
    assert(!decoder.in_progress());
    for (const auto noise : std::array<std::uint8_t, 8>{'l', 'o', 'g', '\n', 'N', 'G', 'X', 'N'}) {
        assert(!decoder.feed(noise));
    }
    bool ready = false;
    for (std::size_t index = 1; index < encoded.size; ++index) {
        ready = decoder.feed(encoded.bytes[index]);
        assert(ready == (index + 1 == encoded.size));
    }
    assert(decoder.payload().size() == status_query.size());
    assert(std::equal(decoder.payload().begin(), decoder.payload().end(),
                      status_query.begin()));
    assert(!decoder.in_progress());

    for (const auto byte : kUsbUpdateEnvelopeMagic) assert(!decoder.feed(byte));
    assert(decoder.in_progress());
    decoder.reset();
    assert(!decoder.in_progress());

    auto corrupt = encoded;
    corrupt.bytes[kUsbUpdateEnvelopeHeaderSize + 2] ^= 0x80;
    for (std::size_t index = 0; index < corrupt.size; ++index) {
        assert(!decoder.feed(corrupt.bytes[index]));
    }
    assert(decoder.rejected_frames() == 1);

    const std::array<std::uint8_t, 7> oversized{
        'N', 'G', 'U', '1', 0xff, 0xff, 0};
    for (const auto byte : oversized) assert(!decoder.feed(byte));
    assert(decoder.rejected_frames() == 2);

    std::vector<std::uint8_t> maximum(kUsbUpdateTransportFrameMaximum, 0xa5);
    maximum[0] = 1;
    maximum[1] = kUpdateDataOpcode;
    const auto max_encoded = encode_usb_update_envelope(maximum);
    assert(max_encoded.size == kUsbUpdateEnvelopeMaximum);
    for (std::size_t index = 0; index < max_encoded.size; ++index) {
        ready = decoder.feed(max_encoded.bytes[index]);
    }
    assert(ready && decoder.payload().size() == maximum.size());
}
