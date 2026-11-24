// Parity checks for the owned-code scanners against the reference
// Foundation/LimbusOwnedCode.h (commit bd32608).
//
// The two scanner entry points accept shapes that the protection library
// itself accepts, and reject the same bounds:
//   * a zero-length range is a valid no-op entry, and a zero-length section
//     hashes to the empty CRC (0) with nothing owned;
//   * the range descriptor budget is 256 entries, matching the protection
//     library's own descriptor count, so 257 entries are rejected.
//
// Only the header-only scanner is exercised here: `integrity.cpp` needs the
// target's ELF/loader APIs, so it is not part of the host target.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "runtime/owned_code.h"

#include "harness.h"

namespace {

using bybus::runtime::owned_code::Patch;
using bybus::runtime::owned_code::Range;

// Same construction as tests/host_smoke.cpp's test_owned_code_ledger(): the
// live buffer carries the post-patch bytes, `before` the original image.
unsigned char live_bytes[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

Patch word_patch() {
    Patch patch;
    patch.offset = 0;
    patch.size = 4;
    patch.substitute = true;
    std::memcpy(patch.before, live_bytes, 4);
    const unsigned char replacement[4] = {0xaa, 0xbb, 0xcc, 0xdd};
    std::memcpy(patch.after, replacement, 4);
    return patch;
}

BYBUS_TEST(empty_section_hashes_to_zero_and_owns_nothing) {
    using bybus::runtime::owned_code::section_crc;
    const Patch patch = word_patch();
    std::uint32_t crc = 0xffffffffu;
    std::size_t owned = 7;
    check(section_crc(live_bytes, 0, 0, {}, crc, owned),
          "parity: a zero-length section is accepted");
    check(crc == 0, "parity: a zero-length section hashes to the empty crc");
    check(owned == 0, "parity: a zero-length section owns nothing");

    // A section that starts and ends at a non-zero offset skips every patch
    // outside it, so an adjacent patch still leaves the section empty.
    crc = 0xffffffffu;
    owned = 7;
    check(section_crc(live_bytes, 8, 0, {patch}, crc, owned),
          "parity: an empty section inside a patched module is accepted");
    check(crc == 0 && owned == 0,
          "parity: an empty section ignores patches outside its bounds");

    // A misaligned size stays rejected: only the zero-length case is allowed.
    crc = 0xffffffffu;
    owned = 0;
    check(!section_crc(live_bytes, 0, 6, {}, crc, owned),
          "parity: a misaligned section size is still rejected");
}

BYBUS_TEST(ranges_accept_a_zero_length_entry) {
    using bybus::runtime::owned_code::ranges_crc;
    std::uint32_t crc = 0;
    std::size_t substituted = 0;
    check(ranges_crc(live_bytes, sizeof(live_bytes), {{0, 0}, {8, 4}}, {}, crc, substituted),
          "parity: a leading zero-length range is accepted");
    check(substituted == 0, "parity: a zero-length range substitutes nothing");
    check(ranges_crc(live_bytes, sizeof(live_bytes), {{0, 4}, {4, 0}}, {}, crc, substituted),
          "parity: a trailing zero-length range is accepted");
    std::uint32_t plain = 0;
    std::size_t plain_substituted = 0;
    check(ranges_crc(live_bytes, sizeof(live_bytes), {{0, 4}}, {}, plain, plain_substituted) &&
              plain == crc,
          "parity: a trailing zero-length range does not change the digest");

    // The zero-length entry does not relax ordering: a later range may not
    // start before the end of the previous one.
    check(!ranges_crc(live_bytes, sizeof(live_bytes), {{8, 4}, {4, 0}}, {}, crc, substituted),
          "parity: zero-length ranges still honour ordering");
}

BYBUS_TEST(ranges_reject_257_entries_and_accept_256) {
    using bybus::runtime::owned_code::ranges_crc;
    // Every entry below is in bounds and properly ordered, so only the
    // descriptor budget can reject the 257-entry set.
    std::vector<unsigned char> module(257, 0x11);
    std::vector<Range> ranges;
    ranges.reserve(257);
    for (std::uintptr_t index = 0; index < 257; ++index) {
        ranges.push_back(Range{index, 1});
    }
    std::uint32_t crc = 0;
    std::size_t substituted = 0;
    check(!ranges_crc(module.data(), module.size(), ranges, {}, crc, substituted),
          "parity: 257 range descriptors exceed the reference budget");

    // Trimming back to the boundary is accepted, so the limit is the only
    // reason for the refusal above.
    ranges.pop_back();
    check(ranges_crc(module.data(), module.size(), ranges, {}, crc, substituted),
          "parity: the 256-entry boundary is inclusive");
    check(substituted == 0, "parity: unpatched descriptors substitute nothing");

    // The same 256 descriptors hash every byte the ranges cover.
    std::uint32_t plain = 0;
    std::size_t plain_substituted = 0;
    check(ranges_crc(module.data(), module.size(), {{0, 256}}, {}, plain, plain_substituted) &&
              plain == crc,
          "parity: 256 single-byte descriptors cover the same bytes as one range");
}

BYBUS_TEST(range_budget_counts_zero_length_entries) {
    using bybus::runtime::owned_code::ranges_crc;
    // Zero-length entries are legal but not free: they consume the same budget.
    std::vector<unsigned char> module(16, 0);
    std::vector<Range> ranges;
    ranges.reserve(257);
    for (std::size_t index = 0; index < 256; ++index) {
        ranges.push_back(Range{0, 0});
    }
    std::uint32_t crc = 0;
    std::size_t substituted = 0;
    check(ranges_crc(module.data(), module.size(), ranges, {}, crc, substituted),
          "parity: 256 zero-length descriptors are accepted");
    ranges.push_back(Range{0, 0});
    check(!ranges_crc(module.data(), module.size(), ranges, {}, crc, substituted),
          "parity: zero-length descriptors count against the budget");
}

} // namespace
