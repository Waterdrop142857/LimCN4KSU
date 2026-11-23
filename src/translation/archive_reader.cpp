// ZIP / 7z reader for the local translation pack importer.
//
// Behaviour baseline: app/src/main/java/com/example/limbuszhcn/translation/
// LocalTranslationArchive.kt (bd32608). Format detection uses the first six
// signature bytes (50 4B -> ZIP, 37 7A BC AF 27 1C -> 7z). Path policy,
// duplicate targets, JSON pairing and the manifest stay with the caller.
//
// ZIP: the central directory drives delivery order (EOCD, then the ZIP64 EOCD
// locator / record and the 0x0001 extra field when a 16 bit field is 0xFFFF or
// a 32 bit field is 0xFFFFFFFF). Only stored (0) and deflate (8) entries are
// accepted; deflate is inflated raw (inflateInit2 with -MAX_WBITS) through the
// NDK zlib and handed to the visitor in 32 KiB chunks, the buffer size the
// Kotlin importer used.
//
// 7z: the vendored LZMA SDK (third_party/lzma-sdk, see VENDOR-RECORD.txt) does
// all header parsing and decoding through its plain C API (SzArEx_Open,
// SzArEx_Extract, SzArEx_GetFileNameUtf16). The solid-block cache of
// SzArEx_Extract (blockIndex / outBuffer / outBufferSize) is kept between
// entries, so one folder is decoded once and later entries are sliced out.
//
// Rules this file enforces:
//   - nothing is written to disk and no path outside the archive is touched;
//   - ZIP data is streamed; 7z solid blocks use the SDK cache;
//   - skipped entries are not extracted;
//   - failures are short ASCII identifiers in ArchiveResult::detail that never
//     contain a path or archive content, and nothing is logged.
//
// Deliberately unsupported (reported, never guessed): encrypted ZIP entries,
// multi-disk archives, ZIP entries whose real sizes are missing from the central
// directory (a data descriptor is fine only because the directory always carries
// the final numbers), ZIP methods other than 0/8, deflate streams that leave
// trailing bytes, and 7z archives that need a coder this SDK build lacks.

#include "translation/archive_reader.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zTypes.h"
#include "shared/utf.h"

namespace bybus::translation {
namespace {

using shared::Code;

constexpr size_t kDeliverChunk = 32u * 1024u;
constexpr size_t kSignatureSize = 6;
constexpr size_t kEocdSize = 22;
constexpr size_t kCentralHeaderSize = 46;
constexpr size_t kLocalHeaderSize = 30;
constexpr size_t kZip64LocatorSize = 20;
constexpr size_t kZip64RecordSize = 56;
constexpr size_t kMaxCommentSize = 65535;
constexpr size_t kLookBufferSize = 64u * 1024u;
constexpr size_t kMaxNameUnits = 4096;

constexpr uint32_t kSigLocalHeader = 0x04034b50u;
constexpr uint32_t kSigCentralHeader = 0x02014b50u;
constexpr uint32_t kSigEndOfCentral = 0x06054b50u;
constexpr uint32_t kSigZip64Locator = 0x07064b50u;
constexpr uint32_t kSigZip64Record = 0x06064b50u;
constexpr uint32_t kSigDigitalSignature = 0x05054b50u;
constexpr uint32_t kSigArchiveExtra = 0x08064b50u;
constexpr uint32_t kAllOnes32 = 0xFFFFFFFFu;
constexpr uint16_t kAllOnes16 = 0xFFFFu;

constexpr uint16_t kZip64ExtraId = 0x0001u;
constexpr uint16_t kZipFlagEncrypted = 0x0001u;
constexpr uint16_t kZipMethodStored = 0;
constexpr uint16_t kZipMethodDeflate = 8;
constexpr uint32_t kDosDirectoryAttr = 0x10u;

constexpr uint8_t kZipPrefix[2] = {0x50, 0x4B};
constexpr uint8_t kSevenZSignature[kSignatureSize] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};

Code fail(ArchiveResult *result, Code code, const char *detail)
{
    result->code = code;
    result->detail = detail;
    return code;
}

Code zip_fail(std::string *detail, const char *text, Code code = Code::ArchiveInvalid)
{
    *detail = text;
    return code;
}

uint16_t read_le16(const uint8_t *src)
{
    const unsigned low = static_cast<unsigned>(src[0]);
    const unsigned high = static_cast<unsigned>(src[1]);
    return static_cast<uint16_t>(low | (high << 8u));
}

uint32_t read_le32(const uint8_t *src)
{
    const uint32_t lo = static_cast<uint32_t>(static_cast<unsigned>(src[0]) |
                                              (static_cast<unsigned>(src[1]) << 8u));
    const uint32_t hi = static_cast<uint32_t>(static_cast<unsigned>(src[2]) |
                                              (static_cast<unsigned>(src[3]) << 8u));
    return lo | (hi << 16u);
}

uint64_t read_le64(const uint8_t *src)
{
    const uint64_t lo = read_le32(src);
    const uint64_t hi = read_le32(src + 4);
    return lo | (hi << 32u);
}

// Owns a read-only descriptor. Every read is an explicit pread, so no file
// position state is shared with the 7z stream callbacks.
class FileHandle {
public:
    FileHandle() = default;
    ~FileHandle() { close(); }
    FileHandle(const FileHandle &) = delete;
    FileHandle &operator=(const FileHandle &) = delete;

    bool open(const std::string &path)
    {
        close();
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        fd_ = fd;
        return true;
    }

    bool size(uint64_t *out) const
    {
        struct stat info {};
        if (fd_ < 0 || ::fstat(fd_, &info) != 0) {
            return false;
        }
        if (!S_ISREG(info.st_mode) || info.st_size < 0) {
            return false;
        }
        *out = static_cast<uint64_t>(info.st_size);
        return true;
    }

    size_t read_at_upto(uint64_t offset, void *buffer, size_t size) const
    {
        if (fd_ < 0) {
            return 0;
        }
        auto *bytes = static_cast<uint8_t *>(buffer);
        size_t done = 0;
        while (done < size) {
            const ssize_t got = ::pread(fd_, bytes + done, size - done,
                                        static_cast<off_t>(offset + done));
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return done;
            }
            if (got == 0) {
                break;
            }
            done += static_cast<size_t>(got);
        }
        return done;
    }

    bool read_at(uint64_t offset, void *buffer, size_t size) const
    {
        return read_at_upto(offset, buffer, size) == size;
    }

private:
    void close()
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd_ = -1;
};

// ---------------------------------------------------------------------------
// ZIP
// ---------------------------------------------------------------------------

struct ZipEndRecord {
    uint64_t entries = 0;
    uint64_t cd_size = 0;
    uint64_t cd_offset = 0;
};

struct ZipEntry {
    std::string name;
    uint16_t flags = 0;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint64_t packed_bytes = 0;
    uint64_t plain_bytes = 0;
    uint64_t local_offset = 0;
    bool directory = false;
};

bool zip_name_is_directory(const std::string &name)
{
    if (name.empty()) {
        return false;
    }
    const char last = name[name.size() - 1];
    return last == '/' || last == '\\';
}

// Locates the end of central directory record and normalises it through the
// ZIP64 locator when any of its six fields did not fit the original layout.
Code locate_end_record(const FileHandle &file, uint64_t file_size, ZipEndRecord *end,
                        std::string *detail)
{
    if (file_size < kEocdSize) {
        return zip_fail(detail, "zip_too_small");
    }
    const uint64_t window = static_cast<uint64_t>(kEocdSize + kMaxCommentSize);
    const uint64_t tail_size = file_size < window ? file_size : window;
    const uint64_t tail_pos = file_size - tail_size;
    std::vector<uint8_t> tail(static_cast<size_t>(tail_size));
    if (!file.read_at(tail_pos, tail.data(), tail.size())) {
        return zip_fail(detail, "zip_read_failed", Code::IoError);
    }

    uint64_t eocd_pos = 0;
    size_t index = tail.size() - kEocdSize;
    for (;;) {
        if (read_le32(&tail[index]) == kSigEndOfCentral) {
            const uint64_t comment = read_le16(&tail[index + 20]);
            const uint64_t candidate = static_cast<uint64_t>(index + kEocdSize) + comment;
            if (candidate <= static_cast<uint64_t>(tail.size())) {
                eocd_pos = tail_pos + index;
                break;
            }
        }
        if (index == 0) {
            return zip_fail(detail, "zip_eocd_missing");
        }
        --index;
    }

    uint8_t record[kEocdSize];
    if (!file.read_at(eocd_pos, record, sizeof record)) {
        return zip_fail(detail, "zip_read_failed", Code::IoError);
    }
    if (read_le32(record) != kSigEndOfCentral) {
        return zip_fail(detail, "zip_eocd_missing");
    }
    if (read_le16(record + 4) != 0 || read_le16(record + 6) != 0) {
        return zip_fail(detail, "zip_multidisk");
    }

    const uint16_t entries16 = read_le16(record + 10);
    const uint32_t cd_size32 = read_le32(record + 12);
    const uint32_t cd_offset32 = read_le32(record + 16);
    end->entries = entries16;
    end->cd_size = cd_size32;
    end->cd_offset = cd_offset32;
    if (entries16 != kAllOnes16 && cd_size32 != kAllOnes32 && cd_offset32 != kAllOnes32) {
        return Code::Ok;
    }

    if (eocd_pos < kZip64LocatorSize) {
        return zip_fail(detail, "zip_zip64_locator_missing");
    }
    uint8_t locator[kZip64LocatorSize];
    if (!file.read_at(eocd_pos - kZip64LocatorSize, locator, sizeof locator)) {
        return zip_fail(detail, "zip_read_failed", Code::IoError);
    }
    if (read_le32(locator) != kSigZip64Locator) {
        return zip_fail(detail, "zip_zip64_locator_missing");
    }
    if (read_le32(locator + 4) != 0 || read_le32(locator + 16) > 1u) {
        return zip_fail(detail, "zip_multidisk");
    }
    const uint64_t record_pos = read_le64(locator + 8);
    if (record_pos > eocd_pos - kZip64LocatorSize) {
        return zip_fail(detail, "zip_zip64_record_invalid");
    }
    uint8_t zip64[kZip64RecordSize];
    if (!file.read_at(record_pos, zip64, sizeof zip64)) {
        return zip_fail(detail, "zip_read_failed", Code::IoError);
    }
    if (read_le32(zip64) != kSigZip64Record || read_le64(zip64 + 4) < 44u) {
        return zip_fail(detail, "zip_zip64_record_invalid");
    }
    if (read_le32(zip64 + 16) != 0 || read_le32(zip64 + 20) != 0) {
        return zip_fail(detail, "zip_multidisk");
    }
    end->entries = read_le64(zip64 + 32);
    end->cd_size = read_le64(zip64 + 40);
    end->cd_offset = read_le64(zip64 + 48);
    return Code::Ok;
}

// Reads the 0x0001 extra field in the order the specification fixes: plain
// size, packed size, local header offset, disk number - only for the fields
// that were saturated in the central directory record.
bool apply_zip64_extra(const uint8_t *extra, size_t extra_size, ZipEntry *entry,
                       std::string *detail)
{
    size_t cursor = 0;
    while (cursor + 4u <= extra_size) {
        const uint16_t id = read_le16(extra + cursor);
        const uint16_t length = read_le16(extra + cursor + 2);
        const size_t body = cursor + 4u;
        if (body + length > extra_size) {
            *detail = "zip_extra_truncated";
            return false;
        }
        if (id == kZip64ExtraId) {
            size_t needed = 0;
            if (entry->plain_bytes == kAllOnes32) {
                needed += 8u;
            }
            if (entry->packed_bytes == kAllOnes32) {
                needed += 8u;
            }
            if (entry->local_offset == kAllOnes32) {
                needed += 8u;
            }
            if (needed > length) {
                *detail = "zip_zip64_extra_missing";
                return false;
            }
            size_t at = body;
            if (entry->plain_bytes == kAllOnes32) {
                entry->plain_bytes = read_le64(extra + at);
                at += 8u;
            }
            if (entry->packed_bytes == kAllOnes32) {
                entry->packed_bytes = read_le64(extra + at);
                at += 8u;
            }
            if (entry->local_offset == kAllOnes32) {
                entry->local_offset = read_le64(extra + at);
            }
            return true;
        }
        cursor = body + length;
    }
    *detail = "zip_zip64_extra_missing";
    return false;
}

Code parse_central_directory(const FileHandle &file, uint64_t file_size,
                             std::vector<ZipEntry> *entries, std::string *detail)
{
    ZipEndRecord end;
    Code code = locate_end_record(file, file_size, &end, detail);
    if (code != Code::Ok) {
        return code;
    }
    if (end.cd_offset > file_size || end.cd_size > file_size - end.cd_offset) {
        return zip_fail(detail, "zip_cd_out_of_range");
    }
    const uint64_t cd_end = end.cd_offset + end.cd_size;
    if (end.entries > (end.cd_size / static_cast<uint64_t>(kCentralHeaderSize))) {
        return zip_fail(detail, "zip_cd_truncated");
    }

    uint64_t cursor = end.cd_offset;
    for (uint64_t index = 0; index < end.entries; ++index) {
        if (cursor > cd_end || cd_end - cursor < static_cast<uint64_t>(kCentralHeaderSize)) {
            return zip_fail(detail, "zip_cd_truncated");
        }
        uint8_t head[kCentralHeaderSize];
        if (!file.read_at(cursor, head, sizeof head)) {
            return zip_fail(detail, "zip_read_failed", Code::IoError);
        }
        if (read_le32(head) != kSigCentralHeader) {
            return zip_fail(detail, "zip_cd_truncated");
        }
        const uint16_t name_len = read_le16(head + 28);
        const uint16_t extra_len = read_le16(head + 30);
        const uint16_t comment_len = read_le16(head + 32);
        const uint64_t var_len = static_cast<uint64_t>(name_len) + extra_len + comment_len;
        if (cd_end - cursor - static_cast<uint64_t>(kCentralHeaderSize) < var_len) {
            return zip_fail(detail, "zip_cd_size_mismatch");
        }
        std::vector<uint8_t> var(static_cast<size_t>(var_len));
        const uint64_t var_pos = cursor + static_cast<uint64_t>(kCentralHeaderSize);
        if (var_len != 0 && !file.read_at(var_pos, var.data(), var.size())) {
            return zip_fail(detail, "zip_read_failed", Code::IoError);
        }
        if (name_len == 0) {
            return zip_fail(detail, "zip_name_empty");
        }

        ZipEntry entry;
        entry.flags = read_le16(head + 8);
        entry.method = read_le16(head + 10);
        entry.crc = read_le32(head + 16);
        entry.packed_bytes = read_le32(head + 20);
        entry.plain_bytes = read_le32(head + 24);
        entry.local_offset = read_le32(head + 42);
        const uint32_t external = read_le32(head + 38);
        entry.name.assign(reinterpret_cast<const char *>(var.data()), name_len);
        if (entry.packed_bytes == kAllOnes32 || entry.plain_bytes == kAllOnes32 ||
            entry.local_offset == kAllOnes32) {
            if (!apply_zip64_extra(var.data() + name_len, extra_len, &entry, detail)) {
                return Code::ArchiveInvalid;
            }
        }
        if ((entry.flags & kZipFlagEncrypted) != 0) {
            return zip_fail(detail, "zip_encrypted");
        }
        if (entry.method != kZipMethodStored && entry.method != kZipMethodDeflate) {
            return zip_fail(detail, "zip_method_unsupported");
        }
        entry.directory = zip_name_is_directory(entry.name) || (external & kDosDirectoryAttr) != 0;
        entries->push_back(std::move(entry));
        cursor += static_cast<uint64_t>(kCentralHeaderSize) + var_len;
    }

    if (cursor < cd_end) {
        // Only the optional records that may legally trail the entry list are
        // accepted here; anything else means the directory size lies.
        uint8_t trailing[4];
        if (cd_end - cursor < 4u || !file.read_at(cursor, trailing, sizeof trailing)) {
            return zip_fail(detail, "zip_cd_size_mismatch");
        }
        const uint32_t signature = read_le32(trailing);
        if (signature != kSigDigitalSignature && signature != kSigArchiveExtra) {
            return zip_fail(detail, "zip_cd_size_mismatch");
        }
    }
    return Code::Ok;
}

// Streams a selected entry's bytes into the visitor and checks arithmetic.
class ZipSink {
public:
    ZipSink(ArchiveVisitor *visitor, uint64_t *total, bool skip)
        : visitor_(visitor), total_(total), skip_(skip)
    {
    }

    Code consume(const uint8_t *data, size_t size, std::string *detail)
    {
        if (size > UINT64_MAX - entry_bytes_ || size > UINT64_MAX - *total_) {
            *detail = "archive_size_overflow";
            return Code::ArchiveLimit;
        }
        entry_bytes_ += size;
        *total_ += size;
        if (skip_) {
            return Code::Ok;
        }
        crc_ = static_cast<uint32_t>(::crc32(crc_, data, static_cast<uInt>(size)));
        if (!visitor_->on_data(reinterpret_cast<const char *>(data), size)) {
            *detail = "visitor_aborted";
            return Code::ArchiveInvalid;
        }
        return Code::Ok;
    }

    uint64_t entry_bytes() const { return entry_bytes_; }
    uint32_t crc() const { return crc_; }

private:
    ArchiveVisitor *visitor_;
    uint64_t *total_;
    bool skip_;
    uint64_t entry_bytes_ = 0;
    uint32_t crc_ = 0;
};

Code stream_stored(const FileHandle &file, uint64_t data_offset, const ZipEntry &entry,
                   ZipSink *sink, std::string *detail)
{
    if (entry.packed_bytes != entry.plain_bytes) {
        return zip_fail(detail, "zip_stored_size_mismatch");
    }
    std::vector<uint8_t> chunk(kDeliverChunk);
    uint64_t left = entry.packed_bytes;
    uint64_t position = data_offset;
    while (left > 0) {
        const size_t step = left < kDeliverChunk ? static_cast<size_t>(left) : kDeliverChunk;
        if (!file.read_at(position, chunk.data(), step)) {
            return zip_fail(detail, "zip_truncated");
        }
        const Code code = sink->consume(chunk.data(), step, detail);
        if (code != Code::Ok) {
            return code;
        }
        position += step;
        left -= step;
    }
    return Code::Ok;
}

Code stream_deflate(const FileHandle &file, uint64_t data_offset, const ZipEntry &entry,
                    ZipSink *sink, std::string *detail)
{
    z_stream stream {};
    if (::inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        return zip_fail(detail, "zip_deflate_init");
    }
    struct InflateGuard {
        z_stream *stream;
        ~InflateGuard() { ::inflateEnd(stream); }
    } guard {&stream};

    std::vector<uint8_t> input(kDeliverChunk);
    std::vector<uint8_t> output(kDeliverChunk);
    uint64_t left = entry.packed_bytes;
    uint64_t position = data_offset;
    for (;;) {
        if (stream.avail_in == 0 && left > 0) {
            const size_t step = left < kDeliverChunk ? static_cast<size_t>(left) : kDeliverChunk;
            if (!file.read_at(position, input.data(), step)) {
                return zip_fail(detail, "zip_truncated");
            }
            position += step;
            left -= step;
            stream.next_in = input.data();
            stream.avail_in = static_cast<uInt>(step);
        }
        stream.next_out = output.data();
        stream.avail_out = static_cast<uInt>(output.size());
        const int status = ::inflate(&stream, Z_NO_FLUSH);
        const size_t produced = output.size() - static_cast<size_t>(stream.avail_out);
        if (produced != 0) {
            const Code code = sink->consume(output.data(), produced, detail);
            if (code != Code::Ok) {
                return code;
            }
        }
        if (status == Z_STREAM_END) {
            break;
        }
        if (status == Z_BUF_ERROR) {
            // No progress: either the input is exhausted and another block has
            // to be fetched, or the stream is broken. Only the first case may
            // continue, so this loop can never spin.
            if (stream.avail_in != 0) {
                return zip_fail(detail, "zip_deflate_error");
            }
            if (left == 0) {
                return zip_fail(detail, "zip_deflate_truncated");
            }
            continue;
        }
        if (status != Z_OK) {
            return zip_fail(detail, "zip_deflate_error");
        }
        if (produced == 0 && left == 0 && stream.avail_in == 0) {
            return zip_fail(detail, "zip_deflate_truncated");
        }
    }
    if (left != 0 || stream.avail_in != 0) {
        return zip_fail(detail, "zip_deflate_trailing");
    }
    return Code::Ok;
}

// Resolves the payload offset from the local file header. Sizes always come from
// the central directory, so a data descriptor is fine as long as the directory
// carries the real numbers; a local header that disagrees on the method is a
// layout this reader refuses to guess about.
bool resolve_local_header(const FileHandle &file, uint64_t file_size, const ZipEntry &entry,
                          uint64_t *data_offset, std::string *detail)
{
    if (entry.local_offset > file_size ||
        file_size - entry.local_offset < static_cast<uint64_t>(kLocalHeaderSize)) {
        *detail = "zip_local_header_invalid";
        return false;
    }
    uint8_t head[kLocalHeaderSize];
    if (!file.read_at(entry.local_offset, head, sizeof head)) {
        *detail = "zip_read_failed";
        return false;
    }
    if (read_le32(head) != kSigLocalHeader) {
        *detail = "zip_local_header_invalid";
        return false;
    }
    if (read_le16(head + 8) != entry.method) {
        *detail = "zip_local_header_mismatch";
        return false;
    }
    const uint64_t name_len = read_le16(head + 26);
    const uint64_t extra_len = read_le16(head + 28);
    const uint64_t offset = entry.local_offset + static_cast<uint64_t>(kLocalHeaderSize) +
                            name_len + extra_len;
    if (offset > file_size || entry.packed_bytes > file_size - offset) {
        *detail = "zip_truncated";
        return false;
    }
    *data_offset = offset;
    return true;
}

Code deliver_zip_entry(const FileHandle &file, uint64_t file_size, const ZipEntry &entry,
                       ArchiveVisitor &visitor, ArchiveResult *result)
{
    ArchiveEntry info;
    info.raw_name = entry.name;
    info.declared_bytes = entry.directory ? 0 : entry.plain_bytes;
    info.directory = entry.directory;
    bool skip = false;
    if (!visitor.on_entry(info, &skip)) {
        return fail(result, Code::ArchiveInvalid, "visitor_aborted");
    }
    ++result->entries;
    if (entry.directory) {
        if (!skip && !visitor.on_entry_end()) {
            return fail(result, Code::ArchiveInvalid, "visitor_aborted");
        }
        return Code::Ok;
    }
    if (skip) return Code::Ok;
    uint64_t data_offset = 0;
    std::string detail;
    if (!resolve_local_header(file, file_size, entry, &data_offset, &detail)) {
        return fail(result, Code::ArchiveInvalid, detail.c_str());
    }
    ZipSink sink(&visitor, &result->bytes, skip);
    const Code code = entry.method == kZipMethodStored
                          ? stream_stored(file, data_offset, entry, &sink, &detail)
                          : stream_deflate(file, data_offset, entry, &sink, &detail);
    if (code != Code::Ok) {
        return fail(result, code, detail.c_str());
    }
    if (sink.entry_bytes() != entry.plain_bytes) {
        return fail(result, Code::ArchiveInvalid, "zip_size_mismatch");
    }
    if (!skip && sink.crc() != entry.crc) {
        return fail(result, Code::ArchiveInvalid, "zip_crc_mismatch");
    }
    if (!skip && !visitor.on_entry_end()) {
        return fail(result, Code::ArchiveInvalid, "visitor_aborted");
    }
    return Code::Ok;
}

Code read_zip(const FileHandle &file, uint64_t file_size,
              ArchiveVisitor &visitor, ArchiveResult *result)
{
    std::vector<ZipEntry> entries;
    std::string detail;
    const Code parsed =
        parse_central_directory(file, file_size, &entries, &detail);
    if (parsed != Code::Ok) {
        return fail(result, parsed, detail.c_str());
    }
    for (const ZipEntry &entry : entries) {
        const Code code = deliver_zip_entry(file, file_size, entry, visitor, result);
        if (code != Code::Ok) {
            return code;
        }
    }
    return Code::Ok;
}

// ---------------------------------------------------------------------------
// 7z (vendored LZMA SDK, C API only)
// ---------------------------------------------------------------------------

// malloc/free behind the SDK interface; the decode path never throws, so a
// failed allocation surfaces as SZ_ERROR_MEM instead of an exception.
void *sz_alloc(ISzAllocPtr self, size_t size)
{
    static_cast<void>(self);
    return std::malloc(size);
}

void sz_free(ISzAllocPtr self, void *address)
{
    static_cast<void>(self);
    std::free(address);
}

const ISzAlloc kSzAlloc = {&sz_alloc, &sz_free};

// ILookInStream over pread. Look hands out a window of the internal buffer,
// Skip advances inside it, Read goes straight to the descriptor and Seek just
// moves the cursor, so no stdio state is shared with the rest of the reader.
class FileLookStream {
public:
    FileLookStream(const FileHandle *file, uint64_t file_size)
        : file_(file), buffer_(kLookBufferSize)
    {
        if (file_size <= static_cast<uint64_t>(INT64_MAX)) {
            size_ = static_cast<Int64>(file_size);
        }
        vt_.Look = &FileLookStream::look_callback;
        vt_.Skip = &FileLookStream::skip_callback;
        vt_.Read = &FileLookStream::read_callback;
        vt_.Seek = &FileLookStream::seek_callback;
    }

    ILookInStream *vtable() { return &vt_; }

private:
    static FileLookStream *self(ILookInStreamPtr stream)
    {
        return reinterpret_cast<FileLookStream *>(const_cast<ILookInStream *>(stream));
    }

    static SRes look_callback(ILookInStreamPtr stream, const void **buf, size_t *size)
    {
        return self(stream)->look(buf, size);
    }
    static SRes skip_callback(ILookInStreamPtr stream, size_t offset)
    {
        return self(stream)->skip(offset);
    }
    static SRes read_callback(ILookInStreamPtr stream, void *buf, size_t *size)
    {
        return self(stream)->read(buf, size);
    }
    static SRes seek_callback(ILookInStreamPtr stream, Int64 *pos, ESzSeek origin)
    {
        return self(stream)->seek(pos, origin);
    }

    bool fill()
    {
        if (pos_ < 0) {
            return false;
        }
        buf_begin_ = pos_;
        buf_len_ = file_->read_at_upto(static_cast<uint64_t>(pos_), buffer_.data(), buffer_.size());
        return buf_len_ != 0;
    }

    SRes look(const void **buf, size_t *size)
    {
        const size_t want = *size;
        if (pos_ < buf_begin_ || pos_ >= buf_begin_ + static_cast<Int64>(buf_len_)) {
            if (!fill()) {
                *buf = nullptr;
                *size = 0;
                return SZ_OK;
            }
        }
        const size_t offset = static_cast<size_t>(pos_ - buf_begin_);
        const size_t available = buf_len_ - offset;
        if (available == 0) {
            *buf = nullptr;
            *size = 0;
            return SZ_OK;
        }
        *buf = buffer_.data() + offset;
        *size = want < available ? want : available;
        return SZ_OK;
    }

    SRes skip(size_t offset)
    {
        if (pos_ < buf_begin_ || pos_ > buf_begin_ + static_cast<Int64>(buf_len_)) {
            return SZ_ERROR_FAIL;
        }
        const size_t offset_in_buffer = static_cast<size_t>(pos_ - buf_begin_);
        if (offset > buf_len_ - offset_in_buffer) {
            return SZ_ERROR_FAIL;
        }
        pos_ += static_cast<Int64>(offset);
        return SZ_OK;
    }

    SRes read(void *buf, size_t *size)
    {
        size_t done = 0;
        if (pos_ >= 0) {
            done = file_->read_at_upto(static_cast<uint64_t>(pos_), buf, *size);
        }
        pos_ += static_cast<Int64>(done);
        buf_begin_ = pos_;
        buf_len_ = 0;
        *size = done;
        return SZ_OK;
    }

    SRes seek(Int64 *pos, ESzSeek origin)
    {
        Int64 target = *pos;
        switch (origin) {
        case SZ_SEEK_SET:
            break;
        case SZ_SEEK_CUR:
            target += pos_;
            break;
        case SZ_SEEK_END:
            target += size_;
            break;
        default:
            return SZ_ERROR_PARAM;
        }
        if (target < 0 || target > size_) {
            return SZ_ERROR_PARAM;
        }
        pos_ = target;
        buf_begin_ = pos_;
        buf_len_ = 0;
        *pos = target;
        return SZ_OK;
    }

    const FileHandle *file_;
    ILookInStream vt_ {};
    std::vector<uint8_t> buffer_;
    Int64 size_ = 0;
    Int64 pos_ = 0;
    Int64 buf_begin_ = 0;
    size_t buf_len_ = 0;
};

// Owns the CSzArEx and the solid-block cache; both are released on every exit
// path, including the throwing ones.
class SevenZipArchive {
public:
    SevenZipArchive() { SzArEx_Init(&db_); }
    ~SevenZipArchive()
    {
        SzArEx_Free(&db_, &kSzAlloc);
        std::free(cache_);
    }
    SevenZipArchive(const SevenZipArchive &) = delete;
    SevenZipArchive &operator=(const SevenZipArchive &) = delete;

    SRes open(ILookInStream *stream)
    {
        // The SDK requires the CRC table once per process before it parses a
        // header; function-local statics give that without any global ctor.
        static const bool crc_table_ready = (CrcGenerateTable(), true);
        static_cast<void>(crc_table_ready);
        return SzArEx_Open(&db_, stream, &kSzAlloc, &kSzAlloc);
    }

    const CSzArEx &db() const { return db_; }
    const Byte *cache() const { return cache_; }
    size_t cache_size() const { return cache_size_; }

    SRes extract(ILookInStream *stream, UInt32 file_index, size_t *offset, size_t *size)
    {
        return SzArEx_Extract(&db_, stream, file_index, &block_index_, &cache_, &cache_size_,
                              offset, size, &kSzAlloc, &kSzAlloc);
    }

private:
    CSzArEx db_ {};
    Byte *cache_ = nullptr;
    size_t cache_size_ = 0;
    UInt32 block_index_ = 0;
};

void map_sz_result(SRes status, Code *code, std::string *detail)
{
    switch (status) {
    case SZ_OK:
        *code = Code::Ok;
        return;
    case SZ_ERROR_NO_ARCHIVE:
        *code = Code::ArchiveInvalid;
        *detail = "7z_not_archive";
        return;
    case SZ_ERROR_ARCHIVE:
        *code = Code::ArchiveInvalid;
        *detail = "7z_bad_archive";
        return;
    case SZ_ERROR_UNSUPPORTED:
        *code = Code::ArchiveInvalid;
        *detail = "7z_unsupported_coder";
        return;
    case SZ_ERROR_CRC:
        *code = Code::ArchiveInvalid;
        *detail = "7z_crc_mismatch";
        return;
    case SZ_ERROR_DATA:
        *code = Code::ArchiveInvalid;
        *detail = "7z_data_error";
        return;
    case SZ_ERROR_INPUT_EOF:
        *code = Code::ArchiveInvalid;
        *detail = "7z_truncated";
        return;
    case SZ_ERROR_OUTPUT_EOF:
        *code = Code::ArchiveInvalid;
        *detail = "7z_output_overflow";
        return;
    case SZ_ERROR_FAIL:
        *code = Code::ArchiveInvalid;
        *detail = "7z_decode_failed";
        return;
    case SZ_ERROR_PARAM:
        *code = Code::ArchiveInvalid;
        *detail = "7z_bad_parameter";
        return;
    case SZ_ERROR_THREAD:
        *code = Code::ArchiveInvalid;
        *detail = "7z_thread_error";
        return;
    case SZ_ERROR_PROGRESS:
        *code = Code::ArchiveInvalid;
        *detail = "7z_progress_abort";
        return;
    case SZ_ERROR_READ:
        *code = Code::IoError;
        *detail = "7z_read_failed";
        return;
    case SZ_ERROR_WRITE:
        *code = Code::IoError;
        *detail = "7z_write_failed";
        return;
    case SZ_ERROR_MEM:
        *code = Code::ArchiveLimit;
        *detail = "7z_mem_limit";
        return;
    default:
        *code = Code::ArchiveInvalid;
        *detail = "7z_decode_failed";
        return;
    }
}

bool seven_zip_name(const CSzArEx &db, UInt32 file_index, std::string *name, std::string *detail)
{
    const size_t needed = SzArEx_GetFileNameUtf16(&db, static_cast<size_t>(file_index), nullptr);
    if (needed == 0 || needed > kMaxNameUnits) {
        *detail = "7z_name_invalid";
        return false;
    }
    std::vector<UInt16> units(needed);
    const size_t written =
        SzArEx_GetFileNameUtf16(&db, static_cast<size_t>(file_index), units.data());
    if (written == 0 || written > needed) {
        *detail = "7z_name_invalid";
        return false;
    }
    static_assert(sizeof(UInt16) == sizeof(uint16_t), "7z name units must be UTF-16 code units");
    const auto *code_units = reinterpret_cast<const uint16_t *>(units.data());
    if (!shared::utf16_to_utf8(code_units, static_cast<int32_t>(written - 1), name)) {
        *detail = "7z_name_invalid";
        return false;
    }
    return true;
}

Code deliver_7z_entry(SevenZipArchive *archive, ILookInStream *stream, UInt32 file_index,
                      ArchiveVisitor &visitor, ArchiveResult *result)
{
    const CSzArEx &db = archive->db();
    std::string name;
    std::string detail;
    if (!seven_zip_name(db, file_index, &name, &detail)) {
        return fail(result, Code::ArchiveInvalid, detail.c_str());
    }
    const size_t index = static_cast<size_t>(file_index);
    const bool directory = db.IsDirs != nullptr && SzArEx_IsDir(&db, index) != 0;
    const uint64_t declared = directory ? 0 : SzArEx_GetFileSize(&db, index);

    ArchiveEntry info;
    info.raw_name = name;
    info.declared_bytes = declared;
    info.directory = directory;
    bool skip = false;
    if (!visitor.on_entry(info, &skip)) {
        return fail(result, Code::ArchiveInvalid, "visitor_aborted");
    }
    ++result->entries;
    if (directory) {
        if (!skip && !visitor.on_entry_end()) {
            return fail(result, Code::ArchiveInvalid, "visitor_aborted");
        }
        return Code::Ok;
    }
    if (skip) return Code::Ok;

    size_t offset = 0;
    size_t processed = 0;
    const SRes status = archive->extract(stream, file_index, &offset, &processed);
    if (status != SZ_OK) {
        Code code = Code::Ok;
        map_sz_result(status, &code, &detail);
        return fail(result, code, detail.c_str());
    }
    if (processed != declared) {
        return fail(result, Code::ArchiveInvalid, "7z_size_mismatch");
    }
    if (offset > archive->cache_size() || processed > archive->cache_size() - offset) {
        return fail(result, Code::ArchiveInvalid, "7z_extract_overflow");
    }
    if (processed == 0) {
        if (!skip && !visitor.on_entry_end()) {
            return fail(result, Code::ArchiveInvalid, "visitor_aborted");
        }
        return Code::Ok;
    }
    if (archive->cache() == nullptr) {
        return fail(result, Code::ArchiveInvalid, "7z_buffer_missing");
    }

    ZipSink sink(&visitor, &result->bytes, skip);
    const Byte *slice = archive->cache() + offset;
    size_t left = processed;
    size_t cursor = 0;
    while (left > 0) {
        const size_t step = left < kDeliverChunk ? left : kDeliverChunk;
        const Code code = sink.consume(slice + cursor, step, &detail);
        if (code != Code::Ok) {
            return fail(result, code, detail.c_str());
        }
        cursor += step;
        left -= step;
    }
    if (!skip && !visitor.on_entry_end()) {
        return fail(result, Code::ArchiveInvalid, "visitor_aborted");
    }
    return Code::Ok;
}

Code read_seven_zip(const FileHandle &file, uint64_t file_size,
                    ArchiveVisitor &visitor, ArchiveResult *result)
{
    FileLookStream stream(&file, file_size);
    SevenZipArchive archive;
    SRes status = archive.open(stream.vtable());
    if (status != SZ_OK) {
        Code code = Code::Ok;
        std::string detail;
        map_sz_result(status, &code, &detail);
        return fail(result, code, detail.c_str());
    }
    const CSzArEx &db = archive.db();
    for (UInt32 index = 0; index < db.NumFiles; ++index) {
        const Code code =
            deliver_7z_entry(&archive, stream.vtable(), index, visitor, result);
        if (code != Code::Ok) {
            return code;
        }
    }
    return Code::Ok;
}

// ---------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------

ArchiveFormat detect_from(const FileHandle &file)
{
    uint8_t head[kSignatureSize] = {0, 0, 0, 0, 0, 0};
    const size_t got = file.read_at_upto(0, head, sizeof head);
    if (got >= 2 && head[0] == kZipPrefix[0] && head[1] == kZipPrefix[1]) {
        return ArchiveFormat::Zip;
    }
    if (got == kSignatureSize && std::memcmp(head, kSevenZSignature, kSignatureSize) == 0) {
        return ArchiveFormat::SevenZ;
    }
    return ArchiveFormat::Unknown;
}

void read_impl(const std::string &path, ArchiveVisitor &visitor,
               ArchiveResult *result)
{
    FileHandle file;
    if (!file.open(path)) {
        fail(result, Code::IoError, "open_failed");
        return;
    }
    uint64_t file_size = 0;
    if (!file.size(&file_size)) {
        fail(result, Code::IoError, "stat_failed");
        return;
    }
    Code code = Code::Ok;
    switch (detect_from(file)) {
    case ArchiveFormat::Zip:
        code = read_zip(file, file_size, visitor, result);
        break;
    case ArchiveFormat::SevenZ:
        code = read_seven_zip(file, file_size, visitor, result);
        break;
    case ArchiveFormat::Unknown:
    default:
        code = fail(result, Code::ArchiveInvalid, "format_unknown");
        break;
    }
    result->code = code;
}

} // namespace

ArchiveFormat detect_format(const std::string &path)
{
    FileHandle file;
    if (!file.open(path)) {
        return ArchiveFormat::Unknown;
    }
    return detect_from(file);
}

ArchiveResult read_archive(const std::string &path, ArchiveVisitor &visitor)
{
    ArchiveResult result;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try {
        read_impl(path, visitor, &result);
    } catch (...) {
        // Nothing in the decode path may unwind through the SDK, but a visitor or
        // a standard container still can; the boundary stays noexcept.
        result.code = Code::InternalError;
        result.detail = "internal_error";
    }
#else
    read_impl(path, visitor, &result);
#endif
    return result;
}

} // namespace bybus::translation
