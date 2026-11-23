#include "translation/appsealing_profile.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "shared/sha256.h"

namespace bybus::translation {
namespace {

constexpr uint64_t kMaxLibraryBytes = 4ull * 1024 * 1024;
constexpr uint64_t kMaxGameLibraryBytes = 512ull * 1024 * 1024;
constexpr uint64_t kProtectionTextVaddr = 0x17130;
constexpr uint64_t kProtectionTextSize = 0x135c90;
constexpr uint64_t kProtectionBssVaddr = 0x1b0bd0;
constexpr uint64_t kProtectionBssSize = 0x4c6f8;
constexpr uint64_t kDescriptorWindowVaddr = 0x1c23e0;
constexpr uint64_t kDescriptorWindowSize = 0x5000;
constexpr uint64_t kProtectionModuleSize = 0x14cdc0;
constexpr uint64_t kRangesVaddr = 0x1c23e0 + 0x2ee0;
constexpr uint32_t kRangesCount = 179;
constexpr uint32_t kDescriptorStride = 0x18;
constexpr const char *kRangesFingerprint = "dd108bf95840fd56";
constexpr const char *kFamily = "limbus-engine1-arm64-full";
constexpr uint32_t kNop = 0xd503201fu;

struct RegionTemplate {
    uint64_t offset;
    uint64_t size;
    const char *sha256;
};

// Immutable code regions shared by the independently inspected 471 and 478
// libraries: PLT; report frame/dispatch; path checks/cleanup; maps; both CRC
// callers; path scan; JNI detach. Encrypted blocks are not wildcard-matched.
constexpr RegionTemplate kRegions[] = {
    {0x16540, 0xbf0, "aa44e10fb224097cc99107e9918d0a6e219bf92080525cd98bdfd02a160481f4"},
    {0x22594, 0xc0, "aac53926ae8bddbf090c0e6723aff8750609d3cc80b7d8c4f06c33a7fb8ce5a1"},
    {0x22b54, 0xcac, "7d0c305364bf5d860cd5d7e16a567ea2a839dea0b835b963211825014b540f0e"},
    {0x4e67c, 0x1984, "a7cc464369613724b4ada3dd0231d04989891e70cd10c5fb5d94636f745fc3ac"},
    {0x50640, 0x9754, "1cf19f5c8789cfe4d67eea4dd2a140ff4874836546db20a822690ef13f3455f3"},
    {0x6fc20, 0x1310, "75379f906f8557abed5258474ea96928fac3ced5dbd50d6b81ed9f6b56b2da9f"},
    {0x99800, 0x100, "ef5c59f710f96869b94ce4750684be27d31350aebb47dcee5149c6a447dd03e8"},
    {0xac878, 0x188, "05973e39d9dd6cd686804d6dbcf7aba79b1966b7d740d9aa9779d4b525c33b10"},
};

constexpr uint32_t kDetachEntryWords[5] = {0xd10043ff, 0xa9007bf3, 0xf0000733, 0xf942de73,
                                           0xaa1303e0};
constexpr uint32_t kReportEntryWords[5] = {0xd12303ff, 0xd10103ff, 0xa90053f3, 0xa9015bf5,
                                           0xa90263f7};
constexpr uint32_t kReportPatternWords[6] = {0xaa0003e1, 0x910fe3e0, 0x52987002,
                                             0x52800023, 0x52800004, 0xd63f0260};

struct GuardWord {
    uint64_t vaddr;
    uint32_t expected;
};

constexpr GuardWord kGuardWords[] = {
    {0x700e4, 0xb942cbe0}, {0x700e8, 0x34000040}, {0x700ec, 0x9400f1e3},
    {0x70b68, 0x9400ef44}, {0x70ba4, 0xd63f00a0}, {0x70ee8, 0xf9416fe0},
    {0x70eec, 0xd1266000}, {0x70128, 0xd63f00a0}, {0x70464, 0x5290d400},
    {0x70468, 0x72a00020},
};

struct Section {
    std::string name;
    uint32_t type = 0;
    uint64_t flags = 0;
    uint64_t vaddr = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
};

struct Segment {
    uint32_t flags = 0;
    uint64_t offset = 0;
    uint64_t vaddr = 0;
    uint64_t filesz = 0;
    uint64_t memsz = 0;
    uint64_t align = 0;
};

uint16_t read_u16(const std::string &bytes, size_t at) {
    uint16_t value = 0;
    if (at + sizeof(value) <= bytes.size()) std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

uint32_t read_u32(const std::string &bytes, size_t at) {
    uint32_t value = 0;
    if (at + sizeof(value) <= bytes.size()) std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

uint64_t read_u64(const std::string &bytes, size_t at) {
    uint64_t value = 0;
    if (at + sizeof(value) <= bytes.size()) std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

class ElfImage {
public:
    bool parse(const std::string &bytes, std::string *error) {
        bytes_ = &bytes;
        sections_.clear();
        segments_.clear();
        if (bytes.size() < 64 || std::memcmp(bytes.data(), "\x7f" "ELF", 4) != 0 ||
            static_cast<uint8_t>(bytes[4]) != 2 || static_cast<uint8_t>(bytes[5]) != 1 ||
            read_u16(bytes, 0x12) != 183 || read_u32(bytes, 0x14) != 1 ||
            read_u16(bytes, 0x36) != 56) {
            *error = "elf_identity";
            return false;
        }
        const uint16_t phnum = read_u16(bytes, 0x38);
        const uint64_t phoff = read_u64(bytes, 0x20);
        if (phnum == 0 || phnum > 64 || phoff > bytes.size() ||
            static_cast<uint64_t>(phnum) * 56 > bytes.size() - phoff) {
            *error = "elf_program_table";
            return false;
        }
        for (uint16_t i = 0; i < phnum; ++i) {
            const size_t row = static_cast<size_t>(phoff) + i * 56;
            if (read_u32(bytes, row) != 1) continue;
            Segment segment;
            segment.flags = read_u32(bytes, row + 4);
            segment.offset = read_u64(bytes, row + 8);
            segment.vaddr = read_u64(bytes, row + 16);
            segment.filesz = read_u64(bytes, row + 32);
            segment.memsz = read_u64(bytes, row + 40);
            segment.align = read_u64(bytes, row + 48);
            if (segment.offset > bytes.size() || segment.filesz > bytes.size() - segment.offset ||
                segment.filesz > segment.memsz || segment.align < 4096 ||
                (segment.align & (segment.align - 1)) != 0) {
                *error = "elf_segment";
                return false;
            }
            segments_.push_back(segment);
        }
        if (segments_.empty()) {
            *error = "elf_segment_missing";
            return false;
        }

        const uint64_t shoff = read_u64(bytes, 0x28);
        const uint16_t shentsize = read_u16(bytes, 0x3A);
        const uint16_t shnum = read_u16(bytes, 0x3C);
        const uint16_t shstrndx = read_u16(bytes, 0x3E);
        if (shentsize != 64 || shnum == 0 || shnum > 256 || shstrndx >= shnum ||
            shoff > bytes.size() || static_cast<uint64_t>(shnum) * 64 > bytes.size() - shoff) {
            *error = "elf_section_table";
            return false;
        }
        const size_t names_row = static_cast<size_t>(shoff) + shstrndx * 64ull;
        const uint64_t names_offset = read_u64(bytes, names_row + 24);
        const uint64_t names_size = read_u64(bytes, names_row + 32);
        if (names_offset > bytes.size() || names_size > bytes.size() - names_offset ||
            names_size > 16 * 1024) {
            *error = "elf_names";
            return false;
        }
        const std::string names = bytes.substr(static_cast<size_t>(names_offset),
                                               static_cast<size_t>(names_size));
        for (uint16_t i = 0; i < shnum; ++i) {
            const size_t row = static_cast<size_t>(shoff) + i * 64ull;
            Section section;
            const uint32_t name_at = read_u32(bytes, row);
            section.type = read_u32(bytes, row + 4);
            section.flags = read_u64(bytes, row + 8);
            section.vaddr = read_u64(bytes, row + 16);
            section.offset = read_u64(bytes, row + 24);
            section.size = read_u64(bytes, row + 32);
            if (name_at < names.size()) {
                const size_t end = names.find('\0', name_at);
                section.name = names.substr(name_at, end == std::string::npos
                                                         ? std::string::npos
                                                         : end - name_at);
            }
            sections_.push_back(std::move(section));
        }
        return true;
    }

    const Section *section(const std::string &name) const {
        for (const Section &section : sections_) {
            if (section.name == name) return &section;
        }
        return nullptr;
    }

    bool mapped(uint64_t vaddr, uint64_t size, uint32_t flags, bool file_backed) const {
        for (const Segment &segment : segments_) {
            if ((segment.flags & 7u) != flags || vaddr < segment.vaddr) continue;
            const uint64_t relative = vaddr - segment.vaddr;
            const uint64_t limit = file_backed ? segment.filesz : segment.memsz;
            if (relative <= limit && size <= limit - relative) return true;
        }
        return false;
    }

    bool vaddr_to_offset(uint64_t vaddr, uint64_t size, uint64_t *offset) const {
        for (const Segment &segment : segments_) {
            if (vaddr < segment.vaddr) continue;
            const uint64_t relative = vaddr - segment.vaddr;
            if (relative > segment.filesz || size > segment.filesz - relative) continue;
            const uint64_t candidate = segment.offset + relative;
            if (candidate <= bytes_->size() && size <= bytes_->size() - candidate) {
                *offset = candidate;
                return true;
            }
        }
        return false;
    }

    bool require_code(const Section &section, std::string *error) const {
        bool related = false;
        for (const Segment &segment : segments_) {
            if (section.vaddr >= segment.vaddr && section.offset >= segment.offset &&
                section.vaddr - segment.vaddr == section.offset - segment.offset &&
                section.offset - segment.offset <= segment.filesz &&
                section.size <= segment.filesz - (section.offset - segment.offset)) {
                related = true;
            }
        }
        if (section.type != 1 || section.flags != 6 || section.size < 4 ||
            (section.size & 3) != 0 || (section.vaddr & 3) != 0 || !related ||
            !mapped(section.vaddr, section.size, 5, true) ||
            section.offset > bytes_->size() || section.size > bytes_->size() - section.offset) {
            *error = "elf_code_section";
            return false;
        }
        return true;
    }

private:
    const std::string *bytes_ = nullptr;
    std::vector<Section> sections_;
    std::vector<Segment> segments_;
};

bool read_word(const std::string &bytes, const ElfImage &elf, uint64_t vaddr, uint32_t *word) {
    uint64_t offset = 0;
    if (!elf.vaddr_to_offset(vaddr, 4, &offset)) return false;
    *word = read_u32(bytes, static_cast<size_t>(offset));
    return true;
}

bool matches(const std::string &bytes, const ElfImage &elf, uint64_t vaddr,
             const uint32_t *expected, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint32_t word = 0;
        if (!read_word(bytes, elf, vaddr + i * 4, &word) || word != expected[i]) return false;
    }
    return true;
}

uint32_t branch_word(uint64_t from, uint64_t to) {
    const int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    if ((delta & 3) != 0 || delta < -0x08000000LL || delta >= 0x08000000LL) return 0;
    return 0x14000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
}

bool qualify_protection(const std::string &bytes, int64_t version_code, AppSealingPlan *plan,
                        std::string *error) {
    if (bytes.empty()) {
        *error = "appsealing_no_protection";
        return false;
    }
    if (bytes.size() > kMaxLibraryBytes) {
        *error = "appsealing_protection_size";
        return false;
    }
    ElfImage elf;
    if (!elf.parse(bytes, error)) return false;
    const Section *text = elf.section(".text");
    const Section *bss = elf.section(".bss");
    if (!text || text->vaddr != kProtectionTextVaddr ||
        text->offset != kProtectionTextVaddr || text->size != kProtectionTextSize ||
        !elf.require_code(*text, error)) {
        *error = "appsealing_protection_text";
        return false;
    }
    if (!bss || bss->type != 8 || bss->vaddr != kProtectionBssVaddr ||
        bss->size != kProtectionBssSize) {
        *error = "appsealing_protection_bss";
        return false;
    }
    if (!elf.mapped(kDescriptorWindowVaddr, kDescriptorWindowSize, 6, false)) {
        *error = "appsealing_protection_mapping";
        return false;
    }
    for (const RegionTemplate &region : kRegions) {
        if (!elf.mapped(region.offset, region.size, 5, true)) {
            *error = "appsealing_protection_mapping";
            return false;
        }
        uint64_t offset = 0;
        if (!elf.vaddr_to_offset(region.offset, region.size, &offset) ||
            shared::Sha256::hex(bytes.substr(static_cast<size_t>(offset),
                                             static_cast<size_t>(region.size))) !=
                region.sha256) {
            *error = "appsealing_region_changed";
            return false;
        }
    }
    if (!matches(bytes, elf, 0xac878, kDetachEntryWords, 5)) {
        *error = "appsealing_entry_changed";
        return false;
    }
    for (const GuardWord &guard : kGuardWords) {
        uint32_t word = 0;
        if (!read_word(bytes, elf, guard.vaddr, &word) || word != guard.expected) {
            *error = "appsealing_guard_changed";
            return false;
        }
    }

    const bool transient_mode = version_code == 471;
    if (!transient_mode) {
        if (!matches(bytes, elf, 0x22594, kReportEntryWords, 5) ||
            !matches(bytes, elf, 0x4e550, kReportPatternWords, 6)) {
            *error = "appsealing_report_changed";
            return false;
        }
    }

    plan->family = kFamily;
    plan->protection_sha256 = shared::Sha256::hex(bytes);
    plan->module_size = kProtectionModuleSize;
    plan->text_vaddr = text->vaddr;
    plan->text_size = text->size;
    plan->text_crc = appsealing_section_crc(
        reinterpret_cast<const uint8_t *>(bytes.data() + text->offset),
        static_cast<size_t>(text->size));
    plan->ranges_vaddr = kRangesVaddr;
    plan->ranges_count = kRangesCount;
    plan->ranges_fingerprint = kRangesFingerprint;

    AppSealingSite data_path;
    data_path.kind = AppSealingSite::Kind::Branch;
    data_path.vaddr = 0x4e6b0;
    data_path.target_vaddr = 0x4e998;
    data_path.substitute = true;
    AppSealingSite path_scan;
    path_scan.kind = AppSealingSite::Kind::Branch;
    path_scan.vaddr = 0x99820;
    path_scan.target_vaddr = 0x99878;
    path_scan.substitute = true;
    AppSealingSite map_report;
    map_report.kind = AppSealingSite::Kind::Nop;
    map_report.vaddr = 0x54488;
    map_report.substitute = true;
    AppSealingSite map_summary;
    map_summary.kind = AppSealingSite::Kind::Nop;
    map_summary.vaddr = 0x545e4;
    map_summary.substitute = true;
    for (AppSealingSite *site : {&data_path, &path_scan, &map_report, &map_summary}) {
        if (!read_word(bytes, elf, site->vaddr, &site->expected)) {
            *error = "appsealing_site_missing";
            return false;
        }
    }
    plan->sites = {data_path, path_scan, map_report, map_summary};

    plan->detach.present = true;
    plan->detach.entry_vaddr = 0xac878;
    plan->detach.expected.assign(kDetachEntryWords, kDetachEntryWords + 5);

    if (!transient_mode) {
        plan->report.present = true;
        plan->report.entry_vaddr = 0x22594;
        plan->report.expected.assign(kReportEntryWords, kReportEntryWords + 5);
        plan->report.code = 50048;
        plan->report.caller_vaddr = 0x4e568;
        plan->report.pattern_vaddr = 0x4e550;
        plan->report.pattern.assign(kReportPatternWords, kReportPatternWords + 6);
        plan->report.continuation_vaddr = 0x4e998;
    } else {
        AppSealingSite transient;
        transient.kind = AppSealingSite::Kind::Branch;
        transient.vaddr = 0x4e564;
        transient.target_vaddr = 0x4e998;
        transient.substitute = false;
        if (!read_word(bytes, elf, transient.vaddr, &transient.expected)) {
            *error = "appsealing_site_missing";
            return false;
        }
        plan->sites.push_back(transient);
        plan->transient.present = true;
        plan->transient.vaddr = transient.vaddr;
        plan->transient.target_vaddr = transient.target_vaddr;
        plan->transient.expected_word = transient.expected;
        plan->transient.branch_word = branch_word(transient.vaddr, transient.target_vaddr);
        plan->transient.sigill_pc_vaddr = 0x4e564;
        plan->transient.sigill_lr_vaddr = 0x4e550;
        if (plan->transient.branch_word == 0) {
            *error = "appsealing_branch_range";
            return false;
        }
    }
    for (AppSealingSite &site : plan->sites) {
        if (site.kind == AppSealingSite::Kind::Branch) {
            site.word = branch_word(site.vaddr, site.target_vaddr);
            if (site.word == 0) {
                *error = "appsealing_branch_range";
                return false;
            }
        } else if (site.kind == AppSealingSite::Kind::Nop) {
            site.word = kNop;
        }
    }

    plan->detach_caller_vaddr = 0x70b6c;
    plan->detach_continuation_vaddr = 0x70ee8;
    plan->confirm_caller_vaddr = 0x700f0;
    plan->confirm_continuation_vaddr = 0x70464;
    plan->stack.record_min_bytes = 0x5d8;
    plan->stack.name_offset = 8;
    plan->stack.text_size_offset = 0x320;
    plan->stack.managed_size_offset = 0x348;
    plan->stack.text_crc_offset = 0x370;
    plan->stack.managed_crc_offset = 0x374;
    plan->stack.caller_stack_min_bytes = 0x700;
    plan->stack.env_offset = 0x580;
    plan->stack.state_offset = 0x2c8;
    plan->stack.index_offset = 0x2cc;
    plan->stack.descriptor_index_offset = 0x658;
    plan->stack.module_offset = 0x6b8;
    plan->stack.expected_offset = 0x648;
    plan->stack.descriptor_count_offset = 8;
    plan->stack.descriptor_entries_offset = kDescriptorStride;
    plan->stack.descriptor_stride = kDescriptorStride;
    plan->report_stable = plan->report.present;
    plan->guards.clear();
    for (const GuardWord &guard : kGuardWords) {
        plan->guards.push_back({guard.vaddr, guard.expected});
    }
    return true;
}

bool qualify_game(const std::string &bytes, AppSealingPlan *plan, std::string *error) {
    if (bytes.size() > kMaxGameLibraryBytes) {
        *error = "appsealing_game_size";
        return false;
    }
    ElfImage elf;
    if (!elf.parse(bytes, error)) return false;
    const Section *text = elf.section(".text");
    const Section *managed = elf.section("il2cpp");
    if (!text || !managed || !elf.require_code(*text, error) ||
        !elf.require_code(*managed, error) ||
        text->vaddr + text->size > managed->vaddr) {
        *error = "appsealing_game_sections";
        return false;
    }
    plan->text_vaddr = text->vaddr;
    plan->text_size = text->size;
    plan->text_crc = appsealing_section_crc(
        reinterpret_cast<const uint8_t *>(bytes.data() + text->offset),
        static_cast<size_t>(text->size));
    plan->managed_vaddr = managed->vaddr;
    plan->managed_size = managed->size;
    plan->managed_crc = appsealing_section_crc(
        reinterpret_cast<const uint8_t *>(bytes.data() + managed->offset),
        static_cast<size_t>(managed->size));
    return true;
}

} // namespace

uint32_t appsealing_section_crc(const uint8_t *data, size_t size) {
    if (!data || size == 0 || (size % 4) != 0) return 0;
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
        }
        table[i] = value;
    }
    uint32_t crc = 0xffffffffu;
    for (size_t position = 0; position < size; position += 4) {
        uint32_t instruction = 0;
        std::memcpy(&instruction, data + position, sizeof(instruction));
        const size_t begin = (instruction == 0xd503201fu || instruction == 0xd5033f5fu) ? 2 : 0;
        for (size_t i = begin; i < 4; ++i) {
            crc = table[(crc ^ data[position + i]) & 0xffu] ^ (crc >> 8);
        }
    }
    return ~crc;
}

uint64_t appsealing_range_fingerprint(const uint8_t *items, size_t count, size_t stride) {
    if (!items || stride < 12) return 0;
    uint64_t hash = 14695981039346656037ull;
    for (size_t index = 0; index < count; ++index) {
        const uint8_t *item = items + index * stride;
        uint64_t offset = 0;
        uint32_t size = 0;
        std::memcpy(&offset, item, sizeof(offset));
        std::memcpy(&size, item + 8, sizeof(size));
        for (const uint64_t value : {offset, static_cast<uint64_t>(size)}) {
            for (unsigned shift = 0; shift < 64; shift += 8) {
                hash ^= (value >> shift) & 0xffu;
                hash *= 1099511628211ull;
            }
        }
    }
    return hash;
}

bool build_appsealing_plan(const std::string &protection, const std::string &il2cpp,
                           int64_t version_code, AppSealingPlan *plan, std::string *error) {
    if (!plan || !error) return false;
    *plan = AppSealingPlan();
    if (protection.empty()) {
        *error = "appsealing_no_protection";
        return false;
    }
    if (il2cpp.empty()) {
        *error = "appsealing_no_il2cpp";
        return false;
    }
    AppSealingPlan compiled;
    if (!qualify_protection(protection, version_code, &compiled, error)) return false;
    if (!qualify_game(il2cpp, &compiled, error)) return false;
    compiled.present = true;
    *plan = std::move(compiled);
    error->clear();
    return true;
}

} // namespace bybus::translation
