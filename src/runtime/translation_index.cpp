#include "runtime/translation_index.h"

#include <cstring>
#include <utility>

namespace bybus::runtime {
namespace {

constexpr unsigned char kMagic[6] = {'L', 'Z', 'T', 'I', '1', 0};
constexpr uint32_t kSchema = 9;
constexpr uint32_t kMaxEntries = 200000;
constexpr uint32_t kMaxString = 64 * 1024;

bool read_u32(const std::string &bytes, size_t *at, uint32_t *value) {
    if (!at || !value || *at > bytes.size() || bytes.size() - *at < 4) return false;
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.data() + *at);
    *value = (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
             (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
    *at += 4;
    return true;
}

bool read_string(const std::string &bytes, size_t *at, std::string *value) {
    uint32_t length = 0;
    if (!read_u32(bytes, at, &length) || length > kMaxString || *at > bytes.size() ||
        bytes.size() - *at < length) {
        return false;
    }
    value->assign(bytes, *at, length);
    *at += length;
    return true;
}

bool read_map(const std::string &bytes, size_t *at,
              std::unordered_map<std::string, std::string> *out) {
    uint32_t count = 0;
    if (!read_u32(bytes, at, &count) || count > kMaxEntries) return false;
    out->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string key;
        std::string value;
        if (!read_string(bytes, at, &key) || !read_string(bytes, at, &value) || key.empty()) {
            return false;
        }
        out->emplace(std::move(key), std::move(value));
    }
    return true;
}

} // namespace

bool TranslationIndex::load(const std::string &bytes, std::string *error) {
    unique_.clear();
    terms_.clear();
    contextual_.clear();
    if (bytes.size() < sizeof(kMagic) + 4) {
        *error = "index_truncated";
        return false;
    }
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        *error = "index_magic";
        return false;
    }
    size_t at = sizeof(kMagic);
    uint32_t schema = 0;
    if (!read_u32(bytes, &at, &schema) || schema != kSchema) {
        *error = "index_schema";
        return false;
    }
    if (!read_map(bytes, &at, &unique_) || !read_map(bytes, &at, &terms_) ||
        !read_map(bytes, &at, &contextual_)) {
        *error = "index_shape";
        return false;
    }
    if (at != bytes.size()) {
        *error = "index_trailing";
        return false;
    }
    return true;
}

const std::string *TranslationIndex::find(const std::string &source) const {
    const auto found = unique_.find(source);
    return found == unique_.end() ? nullptr : &found->second;
}

const std::string *TranslationIndex::find_context(const std::string &key) const {
    const auto found = contextual_.find(key);
    return found == contextual_.end() ? nullptr : &found->second;
}

const std::string *TranslationIndex::find_term(const std::string &source) const {
    const auto found = terms_.find(source);
    return found == terms_.end() ? nullptr : &found->second;
}

std::string translation_context_key(const std::string &id, const std::string &field,
                                    const std::string &source) {
    if (id.empty() || id.size() > 256) return std::string();
    for (const char c : id) {
        if (static_cast<unsigned char>(c) < 0x20) return std::string();
    }
    return id + '\x1f' + field + '\x1f' + source;
}

} // namespace bybus::runtime
