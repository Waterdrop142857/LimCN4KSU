#include "service/android_manifest.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "translation/archive_reader.h"

namespace bybus::service {
namespace {

constexpr uint16_t kXmlType = 0x0003;
constexpr uint16_t kStringPoolType = 0x0001;
constexpr uint16_t kStartElementType = 0x0102;
constexpr uint32_t kNoIndex = 0xffffffffu;
// A UTF-8 string length prefix declares at most four continuation bytes; a
// longer chain is malformed, not merely truncated.
constexpr size_t kMaxLengthContinuations = 4;

struct StringPool {
    std::vector<std::string> strings;
};

uint16_t le16(const std::string &bytes, size_t at) {
    if (at + 2 > bytes.size()) return 0;
    return static_cast<uint16_t>(static_cast<uint8_t>(bytes[at])) |
           (static_cast<uint16_t>(static_cast<uint8_t>(bytes[at + 1])) << 8);
}

uint32_t le32(const std::string &bytes, size_t at) {
    if (at + 4 > bytes.size()) return 0;
    return static_cast<uint32_t>(static_cast<uint8_t>(bytes[at])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 3])) << 24);
}

bool read_utf8_length(const std::string &bytes, size_t *at, uint32_t *length) {
    if (*at >= bytes.size()) return false;
    uint8_t first = static_cast<uint8_t>(bytes[*at]);
    if ((first & 0x80) == 0) {
        *length = first;
        ++*at;
        return true;
    }
    if ((first & 0xC0) == 0x80) return false;
    uint32_t value = first & 0x7f;
    size_t count = 1;
    while (first & 0x80) {
        if (count > kMaxLengthContinuations || *at + count >= bytes.size()) return false;
        value = (value << 8) | static_cast<uint8_t>(bytes[*at + count]);
        first = static_cast<uint8_t>(first << 1);
        ++count;
    }
    *length = value;
    *at += count;
    return true;
}

bool parse_string_pool(const std::string &bytes, size_t chunk, StringPool *pool,
                       std::string *error) {
    const uint16_t header_size = le16(bytes, chunk + 2);
    const uint32_t chunk_size = le32(bytes, chunk + 4);
    const uint32_t count = le32(bytes, chunk + 8);
    const uint32_t flags = le32(bytes, chunk + 16);
    const uint32_t strings_start = le32(bytes, chunk + 20);
    if (header_size < 28 || chunk_size < header_size || chunk + chunk_size > bytes.size() ||
        count > 100000 || chunk + strings_start > bytes.size()) {
        *error = "manifest_string_pool";
        return false;
    }
    const bool utf8 = (flags & 0x100u) != 0;
    size_t offsets = chunk + header_size;
    if (offsets + count * 4ull > chunk + chunk_size) {
        *error = "manifest_string_offsets";
        return false;
    }
    pool->strings.clear();
    pool->strings.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t relative = le32(bytes, offsets + i * 4ull);
        size_t at = chunk + strings_start + relative;
        if (at >= bytes.size()) {
            *error = "manifest_string_bounds";
            return false;
        }
        if (utf8) {
            uint32_t utf16_length = 0;
            uint32_t byte_length = 0;
            if (!read_utf8_length(bytes, &at, &utf16_length) ||
                !read_utf8_length(bytes, &at, &byte_length) ||
                byte_length > bytes.size() - at) {
                *error = "manifest_string_utf8";
                return false;
            }
            pool->strings.push_back(bytes.substr(at, byte_length));
        } else {
            const uint16_t length = le16(bytes, at);
            at += 2;
            if (length > 65535 || at + static_cast<size_t>(length) * 2 > bytes.size()) {
                *error = "manifest_string_utf16";
                return false;
            }
            std::string value;
            value.reserve(length);
            for (uint16_t j = 0; j < length; ++j) {
                const uint16_t unit = le16(bytes, at + j * 2);
                if (unit < 0x80) {
                    value.push_back(static_cast<char>(unit));
                } else if (unit < 0x800) {
                    value.push_back(static_cast<char>(0xc0 | (unit >> 6)));
                    value.push_back(static_cast<char>(0x80 | (unit & 0x3f)));
                } else {
                    value.push_back(static_cast<char>(0xe0 | (unit >> 12)));
                    value.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3f)));
                    value.push_back(static_cast<char>(0x80 | (unit & 0x3f)));
                }
            }
            pool->strings.push_back(std::move(value));
        }
    }
    return true;
}

const std::string &string_at(const StringPool &pool, uint32_t index) {
    static const std::string empty;
    return index == kNoIndex || index >= pool.strings.size() ? empty : pool.strings[index];
}

struct Attribute {
    std::string namespace_uri;
    std::string name;
    std::string value;
};

bool parse_start_element(const std::string &bytes, size_t chunk, const StringPool &pool,
                         std::string *element, std::vector<Attribute> *attributes) {
    const uint16_t header_size = le16(bytes, chunk + 2);
    const uint32_t chunk_size = le32(bytes, chunk + 4);
    if (header_size < 16 || chunk_size < header_size || chunk + chunk_size > bytes.size()) {
        return false;
    }
    const uint32_t name_index = le32(bytes, chunk + 20);
    *element = string_at(pool, name_index);
    const uint16_t attribute_start = le16(bytes, chunk + 24);
    const uint16_t attribute_size = le16(bytes, chunk + 26);
    const uint16_t attribute_count = le16(bytes, chunk + 28);
    if (attribute_size < 20 || attribute_start > chunk_size ||
        16ull + attribute_start +
                static_cast<uint64_t>(attribute_size) * attribute_count >
            chunk_size) {
        return false;
    }
    attributes->clear();
    for (uint16_t i = 0; i < attribute_count; ++i) {
        const size_t at = chunk + 16 + attribute_start + static_cast<size_t>(i) * attribute_size;
        Attribute attribute;
        attribute.namespace_uri = string_at(pool, le32(bytes, at));
        attribute.name = string_at(pool, le32(bytes, at + 4));
        const uint32_t raw = le32(bytes, at + 8);
        const uint8_t data_type = static_cast<uint8_t>(bytes[at + 15]);
        const uint32_t data = le32(bytes, at + 16);
        if (raw != kNoIndex) {
            attribute.value = string_at(pool, raw);
        } else if (data_type == 0x03) {
            attribute.value = string_at(pool, data);
        }
        attributes->push_back(std::move(attribute));
    }
    return true;
}

class ManifestVisitor final : public bybus::translation::ArchiveVisitor {
public:
    bool on_entry(const bybus::translation::ArchiveEntry &entry, bool *skip) override {
        active_ = !entry.directory && entry.raw_name == "AndroidManifest.xml";
        *skip = !active_;
        return true;
    }
    bool on_data(const char *data, size_t size) override {
        if (active_) manifest_.append(data, size);
        return true;
    }
    bool on_entry_end() override {
        active_ = false;
        return true;
    }
    const std::string &manifest() const { return manifest_; }

private:
    bool active_ = false;
    std::string manifest_;
};

} // namespace

AndroidManifestInfo read_android_manifest(const std::string &apk_path) {
    AndroidManifestInfo result;
    ManifestVisitor visitor;
    const bybus::translation::ArchiveResult read =
        bybus::translation::read_archive(apk_path, visitor);
    if (read.code != bybus::shared::Code::Ok) {
        result.error = "manifest_archive";
        return result;
    }
    const std::string &bytes = visitor.manifest();
    if (bytes.size() < 8 || le16(bytes, 0) != kXmlType || le16(bytes, 2) < 8 ||
        le32(bytes, 4) > bytes.size()) {
        result.error = "manifest_header";
        return result;
    }
    const uint32_t total = le32(bytes, 4);
    size_t at = le16(bytes, 2);
    StringPool pool;
    bool parsed_pool = false;
    while (at + 8 <= total) {
        const uint16_t type = le16(bytes, at);
        const uint16_t header_size = le16(bytes, at + 2);
        const uint32_t chunk_size = le32(bytes, at + 4);
        if (header_size < 8 || chunk_size < header_size || at + chunk_size > total) break;
        if (type == kStringPoolType) {
            std::string error;
            if (!parse_string_pool(bytes, at, &pool, &error)) {
                result.error = error;
                return result;
            }
            parsed_pool = true;
        } else if (type == kStartElementType && parsed_pool) {
            std::string element;
            std::vector<Attribute> attributes;
            if (parse_start_element(bytes, at, pool, &element, &attributes)) {
                if (element == "manifest") {
                    for (const Attribute &attribute : attributes) {
                        if (attribute.name == "package") result.package_name = attribute.value;
                    }
                } else if (element == "application") {
                    for (const Attribute &attribute : attributes) {
                        if (attribute.name == "name") result.application_class = attribute.value;
                    }
                    result.ok = true;
                    return result;
                }
            }
        }
        at += chunk_size;
    }
    result.error = "manifest_application";
    return result;
}

} // namespace bybus::service
