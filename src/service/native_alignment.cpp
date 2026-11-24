#include "service/native_alignment.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "translation/archive_reader.h"

namespace bybus::service {
namespace {

constexpr size_t kHeaderBytes = 16 * 1024;
constexpr uint32_t kMaxLibraries = 128;

struct LibraryHeader {
    std::string apk;
    uint64_t file_size = 0;
    std::string header;
};

class AlignmentVisitor final : public bybus::translation::ArchiveVisitor {
public:
    explicit AlignmentVisitor(std::string apk) : apk_(std::move(apk)) {}

    bool on_entry(const bybus::translation::ArchiveEntry &entry, bool *skip) override {
        active_.clear();
        header_.clear();
        file_size_ = entry.declared_bytes;
        if (entry.directory || entry.raw_name.rfind("lib/arm64-v8a/", 0) != 0 ||
            entry.raw_name.size() < 4 ||
            entry.raw_name.compare(entry.raw_name.size() - 3, 3, ".so") != 0) {
            *skip = true;
            return true;
        }
        const std::string name = entry.raw_name.substr(std::string("lib/arm64-v8a/").size());
        if (name.empty() || name.find('/') != std::string::npos) {
            *skip = true;
            return true;
        }
        active_ = name;
        *skip = false;
        return true;
    }

    bool on_data(const char *data, size_t size) override {
        if (active_.empty()) return true;
        const size_t room = kHeaderBytes - header_.size();
        if (room != 0) header_.append(data, std::min(room, size));
        return true;
    }

    bool on_entry_end() override {
        if (!active_.empty()) {
            libraries_[active_] = LibraryHeader{apk_, file_size_, header_};
        }
        active_.clear();
        header_.clear();
        return true;
    }

    const std::map<std::string, LibraryHeader> &libraries() const { return libraries_; }
    const std::string &error() const { return error_; }

private:
    std::string apk_;
    std::string active_;
    std::string header_;
    uint64_t file_size_ = 0;
    std::map<std::string, LibraryHeader> libraries_;
    std::string error_;
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

bool validate_header(const LibraryHeader &library, uint64_t page_size, std::string *error) {
    const std::string &bytes = library.header;
    if (bytes.size() < 64 || std::memcmp(bytes.data(), "\x7f" "ELF", 4) != 0 ||
        static_cast<uint8_t>(bytes[4]) != 2 || static_cast<uint8_t>(bytes[5]) != 1 ||
        static_cast<uint8_t>(bytes[6]) != 1 || read_u16(bytes, 16) != 3 ||
        read_u16(bytes, 18) != 183 || read_u32(bytes, 20) != 1 ||
        read_u16(bytes, 52) != 64) {
        *error = "native_elf_identity";
        return false;
    }
    const uint16_t phnum = read_u16(bytes, 56);
    const uint64_t phoff = read_u64(bytes, 32);
    if (read_u16(bytes, 54) != 56 || phnum == 0 || phnum > 64 || phoff < 64 ||
        phoff > bytes.size() || static_cast<uint64_t>(phnum) * 56 > bytes.size() - phoff) {
        *error = "native_program_table";
        return false;
    }
    struct Segment {
        uint64_t offset;
        uint64_t vaddr;
        uint64_t filesz;
        uint64_t memsz;
        uint64_t align;
    };
    std::vector<Segment> loads;
    for (uint16_t i = 0; i < phnum; ++i) {
        const size_t row = static_cast<size_t>(phoff) + i * 56ull;
        if (read_u32(bytes, row) != 1) continue;
        Segment segment;
        segment.offset = read_u64(bytes, row + 8);
        segment.vaddr = read_u64(bytes, row + 16);
        segment.filesz = read_u64(bytes, row + 32);
        segment.memsz = read_u64(bytes, row + 40);
        segment.align = read_u64(bytes, row + 48);
        if (segment.offset > library.file_size ||
            segment.filesz > library.file_size - segment.offset ||
            segment.filesz > segment.memsz || segment.align < page_size ||
            (segment.align & (segment.align - 1)) != 0 ||
            (segment.vaddr % segment.align) != (segment.offset % segment.align)) {
            *error = "native_segment";
            return false;
        }
        loads.push_back(segment);
    }
    if (loads.empty()) {
        *error = "native_segment_missing";
        return false;
    }
    for (size_t i = 0; i < loads.size(); ++i) {
        for (size_t j = i + 1; j < loads.size(); ++j) {
            const Segment &a = loads[i];
            const Segment &b = loads[j];
            if (a.vaddr + a.memsz <= b.vaddr || b.vaddr + b.memsz <= a.vaddr) continue;
            *error = "native_segment_overlap";
            return false;
        }
    }
    return true;
}

} // namespace

NativeAlignmentResult validate_native_alignment(const std::vector<std::string> &apk_paths,
                                                uint64_t page_size) {
    NativeAlignmentResult result;
    if (page_size != 4096 && page_size != 16384) {
        result.error = "native_page_size";
        return result;
    }
    if (apk_paths.empty()) {
        result.error = "native_no_apk";
        return result;
    }
    std::map<std::string, LibraryHeader> libraries;
    for (const std::string &apk : apk_paths) {
        AlignmentVisitor visitor(apk);
        const bybus::translation::ArchiveResult read =
            bybus::translation::read_archive(apk, visitor);
        if (read.code != bybus::shared::Code::Ok) {
            result.error = "native_archive_" + read.detail;
            return result;
        }
        for (const auto &item : visitor.libraries()) {
            if (libraries.find(item.first) != libraries.end()) {
                result.error = "native_duplicate";
                return result;
            }
            libraries.emplace(item.first, item.second);
            if (libraries.size() > kMaxLibraries) {
                result.error = "native_count";
                return result;
            }
        }
    }
    for (const auto &item : libraries) {
        std::string error;
        if (!validate_header(item.second, page_size, &error)) {
            result.error = error;
            return result;
        }
    }
    for (const char *required : {"libil2cpp.so", "libunity.so", "libmain.so"}) {
        if (libraries.find(required) == libraries.end()) {
            result.error = "native_required";
            return result;
        }
    }
    result.libraries = static_cast<uint32_t>(libraries.size());
    result.ok = true;
    return result;
}

} // namespace bybus::service
