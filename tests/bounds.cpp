// Host tests for the hostile-input bounds of the APK inspection services:
// the AXML string-length prefix, the APK Signing Block size arithmetic and the
// ELF program-header overlap test. Every fixture is a malformed archive that a
// real installer could be handed, so the assertions are about staying inside
// the buffer and failing closed, never about a particular message.
//
// android_manifest / native_alignment consume entries through
// translation/archive_reader, whose implementation is POSIX-only (unistd.h,
// zlib, 7z) and does not build with MSVC. This file therefore carries the host
// double of bybus::translation::read_archive at the bottom: it reads the tiny
// "BTHE" fixture container and delivers exactly the entries it declares. The
// double must be deleted if archive_reader.cpp is ever added to the host build.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "service/android_manifest.h"
#include "service/apk_signature.h"
#include "service/native_alignment.h"
#include "shared/status.h"
#include "translation/archive_reader.h"

#include "harness.h"

namespace {

constexpr char kFixtureMagic[4] = {'B', 'T', 'H', 'E'};

// Little-endian writers, matching the on-disk AXML / APK Signing Block layout.
void put_u16(std::string *out, uint16_t value) {
    out->push_back(static_cast<char>(value & 0xffu));
    out->push_back(static_cast<char>((value >> 8) & 0xffu));
}

void put_u32(std::string *out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out->push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

void set_u16(std::string *out, size_t at, uint16_t value) {
    for (int i = 0; i < 2; ++i) {
        (*out)[at + static_cast<size_t>(i)] =
            static_cast<char>((value >> (i * 8)) & 0xffu);
    }
}

void set_u32(std::string *out, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        (*out)[at + static_cast<size_t>(i)] =
            static_cast<char>((value >> (i * 8)) & 0xffu);
    }
}

void set_u64(std::string *out, size_t at, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        (*out)[at + static_cast<size_t>(i)] =
            static_cast<char>((value >> (i * 8)) & 0xffu);
    }
}

struct FixtureEntry {
    std::string name;
    std::string data;
};

std::string make_container(const std::vector<FixtureEntry> &entries) {
    std::string out(kFixtureMagic, sizeof(kFixtureMagic));
    put_u32(&out, static_cast<uint32_t>(entries.size()));
    for (const FixtureEntry &entry : entries) {
        out.push_back('\0');
        put_u32(&out, static_cast<uint32_t>(entry.name.size()));
        out.append(entry.name);
        put_u32(&out, static_cast<uint32_t>(entry.data.size()));
        out.append(entry.data);
    }
    return out;
}

bool write_file(const std::string &path, const std::string &bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    return static_cast<bool>(file);
}

bool read_file(const std::string &path, std::string *bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamoff size = file.tellg();
    if (size < 0) return false;
    bytes->assign(static_cast<size_t>(size), '\0');
    file.seekg(0, std::ios::beg);
    if (size != 0) file.read(bytes->data(), size);
    return file.gcount() == size;
}

// Writes one fixture container into the current directory (the host runner
// starts the binary inside build/out) and returns its path.
std::string write_fixture(const char *name, const std::vector<FixtureEntry> &entries) {
    if (!write_file(name, make_container(entries))) return std::string();
    return std::string(name);
}

// AXML ----------------------------------------------------------------------

// One binary XML document holding a single UTF-8 string pool whose first string
// uses `prefix` instead of a well-formed [utf16 length][byte length] pair.
std::string make_axml(const std::string &prefix) {
    const size_t strings_start = 28 + 4;
    std::string chunk;
    put_u16(&chunk, 0x0001); // RES_STRING_POOL_TYPE
    put_u16(&chunk, 28);     // header size
    put_u32(&chunk, static_cast<uint32_t>(strings_start + prefix.size()));
    put_u32(&chunk, 1);      // string count
    put_u32(&chunk, 0);      // style count
    put_u32(&chunk, 0x100);  // UTF-8 flag
    put_u32(&chunk, static_cast<uint32_t>(strings_start));
    put_u32(&chunk, 0);      // styles start
    put_u32(&chunk, 0);      // offset table entry 0
    chunk.append(prefix);

    std::string out;
    put_u16(&out, 0x0003); // RES_XML_TYPE
    put_u16(&out, 8);      // header size
    put_u32(&out, static_cast<uint32_t>(8 + chunk.size()));
    out.append(chunk);
    return out;
}

struct AxmlCase {
    const char *what;
    const char *bytes;
    size_t size;
};

// ELF -----------------------------------------------------------------------

constexpr uint64_t kElfModuleBytes = 0x4000;
constexpr uint64_t kElfPageSize = 4096;

struct LoadFixture {
    uint64_t offset;
    uint64_t vaddr;
    uint64_t filesz;
    uint64_t memsz;
};

std::string make_elf(const std::vector<LoadFixture> &loads) {
    const size_t phnum = loads.size();
    std::string out(64 + phnum * 56, '\0');
    out[0] = '\x7f';
    out[1] = 'E';
    out[2] = 'L';
    out[3] = 'F';
    out[4] = static_cast<char>(2); // ELFCLASS64
    out[5] = static_cast<char>(1); // little endian
    out[6] = static_cast<char>(1); // version
    set_u16(&out, 16, 3);          // ET_DYN
    set_u16(&out, 18, 183);        // EM_AARCH64
    set_u32(&out, 20, 1);          // version
    set_u64(&out, 32, 64);         // e_phoff
    set_u16(&out, 52, 64);         // e_ehsize
    set_u16(&out, 54, 56);         // e_phentsize
    set_u16(&out, 56, static_cast<uint16_t>(phnum));
    for (size_t i = 0; i < phnum; ++i) {
        const size_t row = 64 + i * 56;
        set_u32(&out, row, 1);            // PT_LOAD
        set_u32(&out, row + 4, 5);        // R+X
        set_u64(&out, row + 8, loads[i].offset);
        set_u64(&out, row + 16, loads[i].vaddr);
        set_u64(&out, row + 24, loads[i].vaddr);
        set_u64(&out, row + 32, loads[i].filesz);
        set_u64(&out, row + 40, loads[i].memsz);
        set_u64(&out, row + 48, kElfPageSize);
    }
    // Declared size of the archive entry, which is the module size the service
    // bounds every program header against.
    out.resize(static_cast<size_t>(kElfModuleBytes), '\0');
    return out;
}

std::vector<LoadFixture> well_formed_loads() {
    return {{0, 0, 0x1000, 0x1000}, {0x1000, 0x1000, 0x100, 0x200}};
}

constexpr char kNativeLibrary[] = "lib/arm64-v8a/libil2cpp.so";
constexpr char kUnityLibrary[] = "lib/arm64-v8a/libunity.so";
constexpr char kMainLibrary[] = "lib/arm64-v8a/libmain.so";

} // namespace

// AXML ----------------------------------------------------------------------

BYBUS_TEST(axml_truncated_length_prefix_is_rejected) {
    using bybus::service::read_android_manifest;
    // 0x80 payload byte with two bytes left declares "five bytes follow";
    // an open multi-byte prefix and an over-long chain (more than the four
    // continuation bytes UTF-8 can use) must fail the same way.
    static const char kShort[] = {'\x80', '\x05'};
    static const char kOpen[] = {'\xE0'};
    static const char kLong[] = {'\xFE', '\x80', '\x80', '\x80',
                                 '\x80', '\x80', '\x80', '\x80', '\x80'};
    const AxmlCase cases[] = {
        {"axml: 0x80 prefix with two bytes remaining is rejected", kShort, sizeof(kShort)},
        {"axml: prefix cut off at the end of the pool is rejected", kOpen, sizeof(kOpen)},
        {"axml: prefix longer than four bytes is rejected", kLong, sizeof(kLong)},
    };
    for (const AxmlCase &item : cases) {
        const std::string axml = make_axml(std::string(item.bytes, item.size));
        const std::string path =
            write_fixture("t6-bounds-axml.bin", {{"AndroidManifest.xml", axml}});
        check(!path.empty(), "axml: fixture written");
        if (path.empty()) continue;
        const bybus::service::AndroidManifestInfo info = read_android_manifest(path);
        check(!info.ok, item.what);
        check(info.error == "manifest_string_utf8", "axml: malformed prefix reports utf8 failure");
    }
}

BYBUS_TEST(axml_well_formed_string_pool_still_parses) {
    using bybus::service::read_android_manifest;
    // [utf16 length = 3][byte length = 3]abc: the pool must still load, so the
    // document only fails later for having no <application> element.
    static const char kValid[] = {'\x03', '\x03', 'a', 'b', 'c'};
    const std::string axml = make_axml(std::string(kValid, sizeof(kValid)));
    const std::string path =
        write_fixture("t6-bounds-axml-valid.bin", {{"AndroidManifest.xml", axml}});
    check(!path.empty(), "axml: valid fixture written");
    if (path.empty()) return;
    const bybus::service::AndroidManifestInfo info = read_android_manifest(path);
    check(!info.ok, "axml: a pool without an application element fails closed");
    check(info.error == "manifest_application",
          "axml: valid string pool reaches the element scan");
}

// APK Signing Block ---------------------------------------------------------

BYBUS_TEST(signing_block_zero_size_is_rejected) {
    using bybus::service::read_apk_signer;
    // 62 bytes: 24-byte footer at 16 (block_size = 0, then the magic) and the
    // EOCD at 40. total is block_size + 8 = 8, so the block is far too small to
    // hold [size][pairs][size][magic] and `block.size() - 24` would underflow.
    std::string bytes(62, '\0');
    std::memcpy(&bytes[24], "APK Sig Block 42", 16);
    set_u32(&bytes, 40, 0x06054b50u);
    set_u32(&bytes, 56, 40);
    const std::string path = "t6-bounds-signature.apk";
    check(write_file(path, bytes), "signature: fixture written");
    const bybus::service::ApkSignatureResult result = read_apk_signer(path);
    check(!result.ok, "signature: zero-size signing block is rejected");
    check(result.error == "signature_block_size",
          "signature: block smaller than the 24-byte window reports the size failure");
}

BYBUS_TEST(signing_block_pair_window_boundary_is_rejected) {
    using bybus::service::read_apk_signer;
    // block_size = 16 is the smallest value that still satisfies the leading and
    // trailing size copies: total becomes 24, the pair window is empty and the
    // scan must report a missing certificate instead of reading past the block.
    std::string bytes(70, '\0');
    set_u32(&bytes, 24, 16);
    std::memcpy(&bytes[32], "APK Sig Block 42", 16);
    set_u32(&bytes, 48, 0x06054b50u);
    set_u32(&bytes, 64, 48);
    const std::string path = "t6-bounds-signature-empty.apk";
    check(write_file(path, bytes), "signature: boundary fixture written");
    const bybus::service::ApkSignatureResult result = read_apk_signer(path);
    check(!result.ok, "signature: empty pair window is rejected");
    check(result.error == "signature_certificate",
          "signature: empty pair window reports the missing certificate");
}

BYBUS_TEST(signing_block_missing_is_reported) {
    using bybus::service::read_apk_signer;
    std::string bytes(62, '\0');
    set_u32(&bytes, 40, 0x06054b50u);
    set_u32(&bytes, 56, 24);
    const std::string path = "t6-bounds-signature-missing.apk";
    check(write_file(path, bytes), "signature: missing-block fixture written");
    const bybus::service::ApkSignatureResult result = read_apk_signer(path);
    check(!result.ok && result.error == "signature_block_missing",
          "signature: absent signing block is reported as missing");
}

// ELF program headers -------------------------------------------------------

BYBUS_TEST(elf_segment_memsz_cannot_escape_the_module) {
    using bybus::service::validate_native_alignment;
    const std::vector<LoadFixture> good = well_formed_loads();
    struct ElfCase {
        const char *what;
        const char *error;
        std::vector<LoadFixture> loads;
    };
    const ElfCase cases[] = {
        {"elf: well-formed loads are accepted", "",
         good},
        {"elf: memsz = UINT64_MAX cannot declare itself disjoint",
         "native_segment",
         {{0, 0, 0x1000, 0x1000},
          {0x1000, 0x1000, 0x100, UINT64_MAX}}},
        {"elf: memsz beyond the module size is refused", "native_segment",
         {{0, 0, 0x1000, 0x1000}, {0x1000, 0x1000, 0x100, 0x8000}}},
        {"elf: vaddr past module_size - memsz is refused", "native_segment",
         {{0, 0, 0x1000, 0x1000}, {0x1000, 0x1000, 0x100, 0x3900}}},
        {"elf: empty memsz is refused", "native_segment",
         {{0, 0, 0x1000, 0x1000}, {0x1000, 0x1000, 0, 0}}},
        {"elf: a genuine overlap is still refused", "native_segment_overlap",
         {{0, 0, 0x1000, 0x1000}, {0x20, 0x20, 0x40, 0x40}}},
    };
    for (const ElfCase &item : cases) {
        const std::vector<FixtureEntry> entries = {
            {kNativeLibrary, make_elf(item.loads)},
            {kUnityLibrary, make_elf(good)},
            {kMainLibrary, make_elf(good)},
        };
        const std::string path = write_fixture("t6-bounds-native.apk", entries);
        check(!path.empty(), item.what);
        if (path.empty()) continue;
        const bybus::service::NativeAlignmentResult result =
            validate_native_alignment({path}, kElfPageSize);
        if (item.error[0] == '\0') {
            check(result.ok, item.what);
            check(result.libraries == 3, "elf: three libraries are counted");
        } else {
            check(!result.ok, item.what);
            check(result.error == item.error, "elf: fixed rejection code is reported");
        }
    }
}

// Host double of translation/archive_reader ---------------------------------
//
// Only the fixture container is understood: a directory with lib/arm64-v8a
// entries is not needed by these services, so `directory` stays false and
// `declared_bytes` is the stored size, exactly like a stored ZIP entry.

namespace bybus::translation {
namespace {

bool take_u32(const std::string &bytes, size_t at, uint32_t *out) {
    if (at + 4 > bytes.size()) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + i])) << (i * 8);
    }
    *out = value;
    return true;
}

} // namespace

ArchiveResult read_archive(const std::string &path, ArchiveVisitor &visitor) {
    ArchiveResult result;
    std::string bytes;
    if (!read_file(path, &bytes)) {
        result.code = shared::Code::IoError;
        result.detail = "fixture_open";
        return result;
    }
    uint32_t count = 0;
    const bool header_ok =
        bytes.size() >= 8 && std::memcmp(bytes.data(), kFixtureMagic, sizeof(kFixtureMagic)) == 0 &&
        take_u32(bytes, 4, &count);
    if (!header_ok) {
        result.code = shared::Code::ArchiveInvalid;
        result.detail = "fixture_header";
        return result;
    }
    size_t at = 8;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t name_length = 0;
        uint32_t data_length = 0;
        if (at + 5 > bytes.size() || !take_u32(bytes, at + 1, &name_length) ||
            at + 5 + name_length + 4 > bytes.size()) {
            result.code = shared::Code::ArchiveInvalid;
            result.detail = "fixture_entry";
            return result;
        }
        const size_t data_at = at + 5 + name_length + 4;
        if (!take_u32(bytes, at + 5 + name_length, &data_length) ||
            data_at + data_length > bytes.size()) {
            result.code = shared::Code::ArchiveInvalid;
            result.detail = "fixture_entry";
            return result;
        }
        ArchiveEntry entry;
        entry.raw_name = bytes.substr(at + 5, name_length);
        entry.declared_bytes = data_length;
        entry.directory = false;
        bool skip = false;
        if (!visitor.on_entry(entry, &skip)) {
            result.code = shared::Code::ArchiveInvalid;
            result.detail = "fixture_rejected";
            return result;
        }
        if (!skip && data_length != 0 &&
            !visitor.on_data(bytes.data() + data_at, data_length)) {
            result.code = shared::Code::ArchiveInvalid;
            result.detail = "fixture_data";
            return result;
        }
        if (!visitor.on_entry_end()) {
            result.code = shared::Code::ArchiveInvalid;
            result.detail = "fixture_end";
            return result;
        }
        at = data_at + data_length;
        ++result.entries;
        result.bytes += data_length;
    }
    result.code = shared::Code::Ok;
    return result;
}

} // namespace bybus::translation
