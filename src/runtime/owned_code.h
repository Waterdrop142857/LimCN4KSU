// Ported from BYBUS `Foundation/LimbusOwnedCode.h` (commit bd32608).
//
// The protection library hashes code ranges with two different scanners:
//  * `section_crc`: 4-byte instruction stream, skipping the first halfword of
//    NOP and CLREX, with only registered patch bytes substituted;
//  * `ranges_crc`: byte-sized disjoint ranges described by the protection
//    library, substituting the same registered bytes without normalisation.
//
// The ledger never broadens an exemption: a byte is substituted only when the
// caller registered exactly that byte range and the live bytes still equal the
// registered post-patch image.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace bybus::runtime::owned_code {

struct Patch {
    uintptr_t offset = 0;
    size_t size = 0;
    unsigned char before[24] = {};
    unsigned char after[24] = {};
    bool substitute = true;
};

struct Range {
    uintptr_t offset = 0;
    size_t size = 0;
};

inline uint64_t range_fingerprint(const std::vector<Range> &ranges) {
    uint64_t hash = 14695981039346656037ull;
    for (const Range &range : ranges) {
        for (const uint64_t value : {static_cast<uint64_t>(range.offset),
                                     static_cast<uint64_t>(range.size)}) {
            for (unsigned shift = 0; shift < 64; shift += 8) {
                hash ^= (value >> shift) & 0xffu;
                hash *= 1099511628211ull;
            }
        }
    }
    return hash;
}

inline void build_crc_table(uint32_t table[256]) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
        }
        table[i] = value;
    }
}

inline bool ranges_crc(const unsigned char *live, size_t module_size,
                       const std::vector<Range> &ranges, const std::vector<Patch> &patches,
                       uint32_t &result, size_t &substituted) {
    result = 0;
    substituted = 0;
    if (!live || ranges.empty() || ranges.size() > 512) return false;
    uintptr_t previous_end = 0;
    for (const Range &range : ranges) {
        if (!range.size || range.offset < previous_end || range.offset > module_size ||
            range.size > module_size - range.offset) {
            return false;
        }
        previous_end = range.offset + range.size;
    }

    std::vector<const Patch *> active;
    active.reserve(patches.size());
    for (const Patch &patch : patches) {
        if (patch.substitute) active.push_back(&patch);
    }
    std::sort(active.begin(), active.end(),
              [](const Patch *left, const Patch *right) { return left->offset < right->offset; });
    previous_end = 0;
    for (const Patch *patch : active) {
        if (!patch->size || patch->size > sizeof(patch->before) || patch->offset < previous_end ||
            patch->offset > module_size || patch->size > module_size - patch->offset ||
            std::memcmp(live + patch->offset, patch->after, patch->size) != 0) {
            return false;
        }
        previous_end = patch->offset + patch->size;
    }

    uint32_t table[256];
    build_crc_table(table);
    uint32_t crc = 0xffffffffu;
    size_t index = 0;
    for (const Range &range : ranges) {
        for (uintptr_t position = range.offset; position < range.offset + range.size; ++position) {
            while (index < active.size() &&
                   active[index]->offset + active[index]->size <= position) {
                ++index;
            }
            unsigned char value = live[position];
            if (index < active.size() && active[index]->offset <= position) {
                value = active[index]->before[position - active[index]->offset];
                ++substituted;
            }
            crc = table[(crc ^ value) & 0xffu] ^ (crc >> 8);
        }
    }
    result = ~crc;
    return true;
}

inline bool section_crc(const unsigned char *live, uintptr_t offset, size_t size,
                        const std::vector<Patch> &patches, uint32_t &result, size_t &owned) {
    result = 0;
    owned = 0;
    if (!live || size == 0 || (size % 4) != 0 || offset > UINTPTR_MAX - size) return false;
    const uintptr_t end = offset + size;

    std::vector<const Patch *> active;
    active.reserve(patches.size());
    for (const Patch &patch : patches) {
        if (!patch.substitute) continue;
        if (patch.offset + patch.size <= offset || patch.offset >= end) continue;
        if (!patch.size || patch.size > sizeof(patch.before) || (patch.size % 4) != 0 ||
            (patch.offset % 4) != 0 || patch.offset < offset || patch.size > end - patch.offset ||
            std::memcmp(live + (patch.offset - offset), patch.after, patch.size) != 0) {
            return false;
        }
        active.push_back(&patch);
    }
    std::sort(active.begin(), active.end(),
              [](const Patch *left, const Patch *right) { return left->offset < right->offset; });
    uintptr_t previous_end = offset;
    for (const Patch *patch : active) {
        if (patch->offset < previous_end) return false;
        previous_end = patch->offset + patch->size;
        ++owned;
    }

    uint32_t table[256];
    build_crc_table(table);
    uint32_t crc = 0xffffffffu;
    size_t index = 0;
    for (size_t position = 0; position < size; position += 4) {
        const uintptr_t current = offset + position;
        while (index < active.size() && active[index]->offset + active[index]->size <= current) {
            ++index;
        }
        const unsigned char *bytes = live + position;
        if (index < active.size() && active[index]->offset <= current &&
            current < active[index]->offset + active[index]->size) {
            bytes = active[index]->before + (current - active[index]->offset);
        }
        uint32_t instruction = 0;
        std::memcpy(&instruction, bytes, sizeof(instruction));
        const size_t begin = (instruction == 0xd503201fu || instruction == 0xd5033f5fu) ? 2 : 0;
        for (size_t i = begin; i < 4; ++i) crc = table[(crc ^ bytes[i]) & 0xffu] ^ (crc >> 8);
    }
    result = ~crc;
    return true;
}

} // namespace bybus::runtime::owned_code
