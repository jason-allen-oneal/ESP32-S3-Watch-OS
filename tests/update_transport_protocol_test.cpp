#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

#include "nightglass/services/update_transport_protocol.hpp"

using namespace nightglass::services;

namespace {
void put_u32(std::uint8_t *data, std::uint32_t value) {
    for (int index = 0; index < 4; ++index) data[index] = value >> (index * 8);
}
}

int main() {
    constexpr char board[] = "nightglass-waveshare-esp32s3r8-2.06-v1.0";
    constexpr char partition[] = "nightglass-32m-r1";
    constexpr char version[] = "2.0.0";
    std::array<std::uint8_t, 256> bytes{};
    bytes[0] = 1;
    bytes[1] = kUpdateBeginOpcode;
    put_u32(bytes.data() + 2, 0x12345678);
    put_u32(bytes.data() + 6, 0x9abcdef0);
    put_u32(bytes.data() + 10, 1);
    put_u32(bytes.data() + 14, 1);
    put_u32(bytes.data() + 18, 3);
    put_u32(bytes.data() + 22, 4096);
    std::fill_n(bytes.begin() + 26, 32, 0xaa);
    bytes[58] = sizeof(board) - 1;
    bytes[59] = sizeof(partition) - 1;
    bytes[60] = sizeof(version) - 1;
    bytes[61] = 8;
    std::size_t offset = 62;
    std::memcpy(bytes.data() + offset, board, sizeof(board) - 1); offset += sizeof(board) - 1;
    std::memcpy(bytes.data() + offset, partition, sizeof(partition) - 1); offset += sizeof(partition) - 1;
    std::memcpy(bytes.data() + offset, version, sizeof(version) - 1); offset += sizeof(version) - 1;
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), 8, 0x30); offset += 8;
    UpdateTransportCommand command{};
    assert(parse_update_transport_frame(std::span(bytes.data(), offset), command));
    assert(command.kind == UpdateTransportCommandKind::begin);
    assert(command.session == 0x9abcdef012345678ULL);
    assert(command.manifest.image_size == 4096);
    assert(std::strcmp(command.manifest.app_version.data(), version) == 0);
    assert(command.signature_size == 8);

    auto corrupt = bytes;
    corrupt[62] = '=';
    assert(!parse_update_transport_frame(std::span(corrupt.data(), offset), command));
    assert(!parse_update_transport_frame(std::span(bytes.data(), offset - 1), command));

    std::array<std::uint8_t, 244> data{};
    data[0] = 1; data[1] = kUpdateDataOpcode;
    put_u32(data.data() + 2, 7); put_u32(data.data() + 10, 123);
    std::fill(data.begin() + 14, data.end(), 0x5a);
    assert(parse_update_transport_frame(data, command));
    assert(command.kind == UpdateTransportCommandKind::data);
    assert(command.data_size == 230 && command.offset == 123);
    data[1] = kUpdateFinishOpcode;
    assert(!parse_update_transport_frame(data, command));

    std::array<std::uint8_t, 10> control{1, kUpdateStatusQueryOpcode, 1, 0, 0, 0,
                                         0, 0, 0, 0};
    assert(parse_update_transport_frame(control, command));
    assert(command.kind == UpdateTransportCommandKind::status);
    control[2] = 0;
    assert(!parse_update_transport_frame(control, command));

    const auto status = encode_update_transport_status(
        {0x9abcdef012345607ULL, 2, 1, 0, 4096, 230});
    assert(status[0] == 1 && status[1] == kUpdateStatusOpcode && status[2] == 7 &&
           status[6] == 0xf0 && status[9] == 0x9a);
    assert(status[14] == 0 && status[15] == 0x10 && status[18] == 0xe6);
    assert(update_stream_position_matches(0x9abcdef012345678ULL,
                                          0x9abcdef012345678ULL, 230, 230,
                                          true, false));
    assert(!update_stream_position_matches(0x9abcdef012345678ULL,
                                           0x9abcdef012345679ULL, 230, 230,
                                           true, false));
    assert(!update_stream_position_matches(0x9abcdef012345678ULL,
                                           0x9abcdef012345678ULL, 230, 229,
                                           true, false));
    assert(!update_stream_position_matches(0x9abcdef012345678ULL,
                                           0x9abcdef012345678ULL, 230, 230,
                                           true, true));
}
