#include "translation/elf_profile.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "shared/sha256.h"
#include "translation/appsealing_profile.h"
#include "translation/archive_reader.h"

namespace bybus::translation {
namespace {

constexpr const char *kIl2CppEntry = "lib/arm64-v8a/libil2cpp.so";
constexpr const char *kProtectionEntry = "lib/arm64-v8a/libcovault-appsec.so";

struct Section {
    std::string name;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t vaddr = 0;
    uint64_t flags = 0;
    uint32_t type = 0;
};

uint32_t read_u32(const std::string &bytes, size_t at) {
    uint32_t value = 0;
    if (at + 4 <= bytes.size()) std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

uint64_t read_u64(const std::string &bytes, size_t at) {
    uint64_t value = 0;
    if (at + 8 <= bytes.size()) std::memcpy(&value, bytes.data() + at, 8);
    return value;
}

uint16_t read_u16(const std::string &bytes, size_t at) {
    uint16_t value = 0;
    if (at + 2 <= bytes.size()) std::memcpy(&value, bytes.data() + at, 2);
    return value;
}

// CRC32 with the checker's NOP/CLREX normalisation applied.
uint32_t section_crc(const std::string &bytes, uint64_t offset, uint64_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset) return 0;
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
        }
        table[i] = value;
    }
    uint32_t crc = 0xffffffffu;
    const auto *data = reinterpret_cast<const uint8_t *>(bytes.data() + offset);
    for (uint64_t position = 0; position + 4 <= size; position += 4) {
        uint32_t instruction = 0;
        std::memcpy(&instruction, data + position, sizeof(instruction));
        const size_t begin =
            (instruction == 0xd503201fu || instruction == 0xd5033f5fu) ? 2 : 0;
        for (size_t i = begin; i < 4; ++i) crc = table[(crc ^ data[position + i]) & 0xffu] ^ (crc >> 8);
    }
    return ~crc;
}

bool parse_sections(const std::string &bytes, std::vector<Section> *sections, std::string *error) {
    if (bytes.size() < 64 || std::memcmp(bytes.data(), "\x7f" "ELF", 4) != 0) {
        *error = "elf_magic";
        return false;
    }
    if (static_cast<uint8_t>(bytes[4]) != 2 || static_cast<uint8_t>(bytes[5]) != 1) {
        *error = "elf_class";
        return false;
    }
    const uint64_t section_offset = read_u64(bytes, 0x28);
    const uint16_t entry_size = read_u16(bytes, 0x3A);
    const uint16_t count = read_u16(bytes, 0x3C);
    const uint16_t names_index = read_u16(bytes, 0x3E);
    if (entry_size != 64 || count == 0 || count > 96) {
        *error = "elf_section_table";
        return false;
    }
    if (section_offset > bytes.size() || static_cast<uint64_t>(count) * entry_size >
                                             bytes.size() - section_offset) {
        *error = "elf_section_bounds";
        return false;
    }
    if (names_index >= count) {
        *error = "elf_names_index";
        return false;
    }
    const size_t names_header = static_cast<size_t>(section_offset) + names_index * entry_size;
    const uint64_t names_offset = read_u64(bytes, names_header + 0x18);
    const uint64_t names_size = read_u64(bytes, names_header + 0x20);
    if (names_offset > bytes.size() || names_size > bytes.size() - names_offset) {
        *error = "elf_names_bounds";
        return false;
    }
    const std::string names = bytes.substr(static_cast<size_t>(names_offset),
                                           static_cast<size_t>(names_size));
    sections->clear();
    for (uint16_t i = 0; i < count; ++i) {
        const size_t header = static_cast<size_t>(section_offset) + i * entry_size;
        Section section;
        const uint32_t name_at = read_u32(bytes, header);
        section.type = read_u32(bytes, header + 4);
        section.flags = read_u64(bytes, header + 8);
        section.vaddr = read_u64(bytes, header + 16);
        section.offset = read_u64(bytes, header + 0x18);
        section.size = read_u64(bytes, header + 0x20);
        if (name_at < names.size()) {
            const size_t end = names.find('\0', name_at);
            section.name = names.substr(name_at, end == std::string::npos ? std::string::npos
                                                                         : end - name_at);
        }
        sections->push_back(std::move(section));
    }
    return true;
}

class LibraryCollector final : public ArchiveVisitor {
public:
    LibraryCollector(std::string il2cpp, std::string protection)
        : il2cpp_entry_(std::move(il2cpp)), protection_entry_(std::move(protection)) {}

    bool on_entry(const ArchiveEntry &entry, bool *skip) override {
        const std::string &wanted =
            entry.raw_name == il2cpp_entry_ ? entry.raw_name
                                            : (entry.raw_name == protection_entry_ ? entry.raw_name
                                                                                  : std::string());
        active_ = wanted;
        if (active_.empty() || entry.directory) {
            *skip = true;
            return true;
        }
        *skip = false;
        return true;
    }

    bool on_data(const char *data, size_t size) override {
        if (active_.empty()) return true;
        std::string &target = active_ == il2cpp_entry_ ? il2cpp_ : protection_;
        if (size > target.max_size() - target.size()) return false;
        target.append(data, size);
        return true;
    }

    bool on_entry_end() override {
        active_.clear();
        return true;
    }

    const std::string &il2cpp() const { return il2cpp_; }
    const std::string &protection() const { return protection_; }

private:
    std::string il2cpp_entry_;
    std::string protection_entry_;
    std::string active_;
    std::string il2cpp_;
    std::string protection_;
};

} // namespace

IntegrityProfileData collect_integrity_profile(const std::string &apk_path,
                                               int64_t version_code) {
    IntegrityProfileData profile;
    if (apk_path.empty()) {
        profile.detail = "profile_no_apk";
        return profile;
    }
    LibraryCollector collector(kIl2CppEntry, kProtectionEntry);
    const ArchiveResult read = read_archive(apk_path, collector);
    if (read.code != shared::Code::Ok) {
        profile.detail = "profile_read_" + read.detail;
        return profile;
    }
    if (collector.il2cpp().empty()) {
        profile.detail = "profile_no_il2cpp";
        return profile;
    }
    std::vector<Section> sections;
    std::string error;
    if (!parse_sections(collector.il2cpp(), &sections, &error)) {
        profile.detail = error;
        return profile;
    }
    const Section *text = nullptr;
    const Section *rodata = nullptr;
    const Section *managed = nullptr;
    for (const Section &section : sections) {
        if (section.name == ".text") text = &section;
        else if (section.name == ".rodata") rodata = &section;
        else if (section.name == "il2cpp") managed = &section;
    }
    if (!text || text->size == 0 || text->offset >= collector.il2cpp().size()) {
        profile.detail = "profile_no_text";
        return profile;
    }
    profile.il2cpp_sha256 = shared::Sha256::hex(collector.il2cpp());
    if (!collector.protection().empty()) {
        profile.protection_sha256 = shared::Sha256::hex(collector.protection());
    }
    profile.text_offset = text->offset;
    profile.text_size = text->size;
    profile.text_crc = section_crc(collector.il2cpp(), text->offset, text->size);
    profile.text_vaddr = text->vaddr;
    if (managed && managed->size != 0 && managed->offset < collector.il2cpp().size()) {
        profile.managed_vaddr = managed->vaddr;
        profile.managed_size = managed->size;
        profile.managed_crc = section_crc(collector.il2cpp(), managed->offset, managed->size);
    }
    if (rodata && rodata->size != 0) {
        profile.rodata_offset = rodata->offset;
        profile.rodata_size = rodata->size;
        profile.rodata_crc = section_crc(collector.il2cpp(), rodata->offset, rodata->size);
    }
    std::string compat_error;
    if (build_appsealing_plan(collector.protection(), collector.il2cpp(), version_code,
                              &profile.appsealing, &compat_error)) {
        profile.appsealing_detail.clear();
    } else {
        profile.appsealing_detail = compat_error;
    }
    profile.present = true;
    profile.detail.clear();
    return profile;
}

} // namespace bybus::translation
