#include "translation/index_compiler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "shared/sha256.h"
#include "shared/utf.h"
#include "translation/text_policy.h"

namespace bybus::translation {
namespace {

constexpr const char *kRevision = "full-locales-18";
constexpr uint32_t kIndexSchema = 9;
constexpr uint32_t kMaxEntries = 200000;
constexpr size_t kMaxStringBytes = 64 * 1024;
constexpr size_t kMaxIndexBytes = 64 * 1024 * 1024;
constexpr const unsigned char kMagic[6] = {'L', 'Z', 'T', 'I', '1', 0};

bool ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trim(std::string text) {
    while (!text.empty() && ascii_space(text.front())) text.erase(text.begin());
    while (!text.empty() && ascii_space(text.back())) text.pop_back();
    return text;
}

std::string lower_ascii(std::string text) {
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::vector<uint32_t> codepoints(const std::string &text) {
    std::vector<uint32_t> result;
    std::u16string units;
    if (!shared::utf8_to_utf16(text, &units)) return result;
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t point = units[i];
        if (point >= 0xd800 && point <= 0xdbff && i + 1 < units.size()) {
            const uint32_t low = units[i + 1];
            if (low >= 0xdc00 && low <= 0xdfff) {
                point = 0x10000 + ((point - 0xd800) << 10) + (low - 0xdc00);
                ++i;
            }
        }
        result.push_back(point);
    }
    return result;
}

bool is_han_label(const std::string &text) {
    bool has_han = false;
    for (const uint32_t point : codepoints(text)) {
        if (point >= 0x3400 && point <= 0x9fff) has_han = true;
        if ((point >= 0x3040 && point <= 0x30ff) || (point >= 0xff66 && point <= 0xff9f)) {
            return false;
        }
    }
    return has_han;
}

bool is_numeric_literal(const std::string &value);

std::string strip_decorative_tags(const std::string &value) {
    std::string lowered = lower_ascii(value);
    if (lowered.find("<noparse") != std::string::npos ||
        lowered.find("</noparse") != std::string::npos) {
        return value;
    }
    static const char *const kNames[] = {
        "b", "i", "u", "s", "color", "alpha", "size", "line-height", "cspace",
        "mspace", "voffset", "mark", "smallcaps", "uppercase", "lowercase", "allcaps"};
    std::string result;
    for (size_t i = 0; i < value.size();) {
        if (value[i] != '<') {
            result.push_back(value[i++]);
            continue;
        }
        const size_t end = value.find('>', i + 1);
        if (end == std::string::npos) {
            result.append(value.substr(i));
            break;
        }
        const std::string tag = lower_ascii(value.substr(i + 1, end - i - 1));
        std::string name = tag;
        if (!name.empty() && name.front() == '/') name.erase(name.begin());
        const size_t equals = name.find_first_of("= \t");
        if (equals != std::string::npos) name.resize(equals);
        bool decorative = false;
        for (const char *candidate : kNames) {
            if (name == candidate) decorative = true;
        }
        if (tag.size() == 7 && tag.front() == '#' &&
            std::all_of(tag.begin() + 1, tag.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            })) {
            decorative = true;
        }
        if (decorative) {
            i = end + 1;
        } else {
            result.append(value.substr(i, end - i + 1));
            i = end + 1;
        }
    }
    return result;
}

std::string strip_paired_brackets(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 3 && value.front() == '[' && value.back() == ']') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool is_numeric_literal(const std::string &value) {
    const std::string stripped = strip_decorative_tags(value);
    std::string text;
    for (const char c : stripped) {
        if (!ascii_space(c)) text.push_back(c);
    }
    if (text.empty() || text.size() > 64) return false;
    bool digit = false;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            digit = true;
        } else if (c != '+' && c != '-' && c != '.' && c != ',' && c != ':' && c != '/' &&
                   c != '%' && c != '(' && c != ')') {
            return false;
        }
    }
    return digit;
}

bool is_term_like(const std::string &value, size_t minimum, bool phrases = false) {
    const std::vector<uint32_t> points = codepoints(value);
    if (phrases && value.find(' ') != std::string::npos) {
        if (points.size() < minimum || points.size() > 64) return false;
        size_t words = 0;
        size_t word_length = 0;
        for (char c : value) {
            if (c == ' ') {
                if (word_length == 0) return false;
                ++words;
                word_length = 0;
                continue;
            }
            const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                 (c >= '0' && c <= '9') || c == '\'' || c == '-' ||
                                 c == '_' || c == '.';
            if (!allowed) return false;
            ++word_length;
        }
        if (word_length == 0 || value.back() == '.') return false;
        ++words;
        return words >= 2 && words <= 8;
    }
    if (points.size() < minimum || points.size() > 16) return false;
    for (const char c : value) {
        if (ascii_space(c) || c == '<' || c == '>' || c == '{' || c == '}' || c == '%') {
            return false;
        }
    }
    return true;
}

std::vector<std::string> bracket_contents(const std::string &value) {
    std::vector<std::string> result;
    for (size_t i = 0; i < value.size();) {
        if (value[i] != '[') {
            ++i;
            continue;
        }
        const size_t end = value.find(']', i + 1);
        if (end == std::string::npos) break;
        if (value.find('[', i + 1) < end) break;
        result.push_back(value.substr(i + 1, end - i - 1));
        i = end + 1;
    }
    return result;
}

std::string leading_clause(const std::string &value) {
    std::string text = value;
    size_t begin = 0;
    while (begin < text.size() && ascii_space(text[begin])) ++begin;
    if (begin < text.size() && (text[begin] == '-' || text[begin] == ',')) {
        ++begin;
        while (begin < text.size() && ascii_space(text[begin])) ++begin;
    }
    text = text.substr(begin);
    // Delimiters: ASCII comma/colon plus UTF-8 Chinese '、', '，', '：'.
    for (size_t i = 0; i < text.size();) {
        size_t width = 1;
        bool delimiter = text[i] == ',' || text[i] == ':';
        if (i + 2 < text.size() && static_cast<unsigned char>(text[i]) == 0xe3 &&
            static_cast<unsigned char>(text[i + 1]) == 0x80 &&
            static_cast<unsigned char>(text[i + 2]) == 0x81) {
            delimiter = true;
            width = 3;
        } else if (i + 2 < text.size() && static_cast<unsigned char>(text[i]) == 0xef &&
                   static_cast<unsigned char>(text[i + 1]) == 0xbc &&
                   (static_cast<unsigned char>(text[i + 2]) == 0x8c ||
                    static_cast<unsigned char>(text[i + 2]) == 0x9a)) {
            delimiter = true;
            width = 3;
        }
        if (delimiter) {
            const std::vector<uint32_t> points = codepoints(text.substr(0, i));
            if (points.size() >= 2 && points.size() <= 32) return text.substr(0, i);
            return std::string();
        }
        i += width;
    }
    return std::string();
}

int term_priority(const std::string &path) {
    const std::string file = lower_ascii(JsonPairer::source_table_name(path));
    if (file.rfind("bufs", 0) == 0 || file.rfind("battlekeywords", 0) == 0) return 3;
    if (file.find("keyword") != std::string::npos) return 2;
    return 1;
}

std::string resolve_translation(const std::map<std::string, uint32_t> &counts) {
    if (counts.size() == 1) return counts.begin()->first;
    std::vector<std::pair<std::string, uint32_t>> ranked(counts.begin(), counts.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &left, const auto &right) {
        if (left.second != right.second) return left.second > right.second;
        return left.first < right.first;
    });
    const uint32_t winner = ranked[0].second;
    const uint32_t runner = ranked.size() > 1 ? ranked[1].second : 0;
    uint64_t total = 0;
    for (const auto &item : counts) total += item.second;
    if (winner >= 3 && static_cast<uint64_t>(winner) * 5 >= total * 4 &&
        winner >= runner * 3) {
        return ranked[0].first;
    }
    return std::string();
}

std::string context_key(const std::string &id, const std::string &field,
                        const std::string &source) {
    if (id.empty() || id.size() > 256) return std::string();
    for (const char c : id) {
        if (static_cast<unsigned char>(c) < 0x20) return std::string();
    }
    return id + '\x1f' + field + '\x1f' + source;
}

// TranslationIndexCompiler.kt:379 STORY_CONTEXT, matched against the whole id:
// ^(@story:[A-Za-z0-9_-]{1,96})(?:/dialog:([0-9]{1,18}))?$
bool is_story_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

bool parse_story_id(const std::string &id, std::string *scene, std::string *dialog) {
    constexpr size_t kPrefixLength = 7; // "@story:"
    constexpr size_t kDialogLength = 8; // "/dialog:"
    if (id.size() <= kPrefixLength || id.compare(0, kPrefixLength, "@story:") != 0) return false;
    size_t at = kPrefixLength;
    const size_t scene_at = at;
    while (at < id.size() && is_story_char(id[at])) ++at;
    if (at - scene_at < 1 || at - scene_at > 96) return false;
    scene->assign(id, scene_at, at - scene_at);
    dialog->clear();
    if (at == id.size()) return true;
    if (id.compare(at, kDialogLength, "/dialog:") != 0) return false;
    at += kDialogLength;
    const size_t digits_at = at;
    while (at < id.size() && id[at] >= '0' && id[at] <= '9') ++at;
    if (at - digits_at < 1 || at - digits_at > 18 || at != id.size()) return false;
    dialog->assign(id, digits_at, at - digits_at);
    return true;
}

std::vector<std::string> record_ids(const IndexRecord &record) {
    std::vector<std::string> ids;
    auto append_distinct = [&ids](const std::string &id) {
        if (id.empty()) return;
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) return;
        ids.push_back(id);
    };
    if (!record.stable_id.empty()) ids.push_back(record.stable_id);
    std::string scene;
    std::string dialog;
    if (parse_story_id(record.stable_id, &scene, &dialog)) {
        append_distinct(scene);
        append_distinct(dialog);
    }
    return ids;
}

void append_u32(std::string *out, uint32_t value) {
    out->push_back(static_cast<char>(value >> 24));
    out->push_back(static_cast<char>(value >> 16));
    out->push_back(static_cast<char>(value >> 8));
    out->push_back(static_cast<char>(value));
}

// Writes one map's pairs in linked-map order and returns how many were written.
// The caller keeps the leading count so the schema-9 header stays consistent with
// the bytes even if an order vector was left stale by a hand-built index.
uint32_t append_pairs(std::string *out, const std::map<std::string, std::string> &entries,
                      const std::vector<std::string> &order) {
    std::set<std::string> written;
    uint32_t count = 0;
    auto append = [out, &count](const std::string &key, const std::string &value) {
        append_u32(out, static_cast<uint32_t>(key.size()));
        out->append(key);
        append_u32(out, static_cast<uint32_t>(value.size()));
        out->append(value);
        ++count;
    };
    for (const std::string &key : order) {
        const auto found = entries.find(key);
        if (found == entries.end() || !written.insert(key).second) continue;
        append(found->first, found->second);
    }
    // Keys missing from the order vector are still content: append them last so
    // nothing is dropped and the count never disagrees with the payload.
    for (const auto &entry : entries) {
        if (!written.insert(entry.first).second) continue;
        append(entry.first, entry.second);
    }
    return count;
}

} // namespace

void put_ordered(std::map<std::string, std::string> *entries, std::vector<std::string> *order,
                 const std::string &key, const std::string &value) {
    if (!entries || !order) return;
    if (entries->find(key) == entries->end()) order->push_back(key);
    (*entries)[key] = value;
}

CompiledTranslationIndex compile_translation_index(
    const std::vector<IndexRecord> &records, const std::string &version,
    const std::string &archive_sha256, uint32_t candidate_source_files,
    uint32_t available_source_files) {
    std::vector<const IndexRecord *> usable;
    usable.reserve(records.size());
    for (const IndexRecord &record : records) {
        // TranslationIndexCompiler.kt:94-103 rejects blank or encoding-damaged
        // display pairs before they can reach any of the three maps.
        if (!is_display_field(record.field) || is_blank(record.source) ||
            is_blank(record.translation) || !is_usable_display_text(record.source) ||
            !is_usable_display_text(record.translation)) {
            continue;
        }
        usable.push_back(&record);
    }
    std::sort(usable.begin(), usable.end(), [](const IndexRecord *left, const IndexRecord *right) {
        if (left->source != right->source) return left->source < right->source;
        if (left->translation != right->translation) return left->translation < right->translation;
        if (left->path != right->path) return left->path < right->path;
        if (left->context != right->context) return left->context < right->context;
        if (left->field != right->field) return left->field < right->field;
        return left->stable_id < right->stable_id;
    });
    usable.erase(std::unique(usable.begin(), usable.end(),
                             [](const IndexRecord *left, const IndexRecord *right) {
                                 return left->source == right->source &&
                                        left->translation == right->translation &&
                                        left->path == right->path &&
                                        left->context == right->context &&
                                        left->field == right->field &&
                                        left->stable_id == right->stable_id;
                             }),
                 usable.end());

    std::set<std::string> source_names;
    std::set<std::string> unchanged_sources;
    std::set<std::string> translated_outputs;
    for (const IndexRecord *record : usable) {
        source_names.insert(record->source);
        if (record->source == record->translation) {
            unchanged_sources.insert(record->source);
        } else {
            translated_outputs.insert(record->translation);
        }
    }

    CompiledTranslationIndex result;
    result.version = version;
    result.archive_sha256 = archive_sha256;
    result.candidate_source_file_count = candidate_source_files;
    result.available_source_file_count = available_source_files;

    size_t begin = 0;
    while (begin < usable.size()) {
        size_t end = begin + 1;
        while (end < usable.size() && usable[end]->source == usable[begin]->source) ++end;
        const std::string &source = usable[begin]->source;
        std::map<std::string, uint32_t> translations;
        for (size_t i = begin; i < end; ++i) ++translations[usable[i]->translation];
        const bool unchanged = unchanged_sources.find(source) != unchanged_sources.end();
        const std::string resolved = unchanged ? source : resolve_translation(translations);
        const bool identity_rule = translations.size() > 1 || is_han_label(source);
        if (!resolved.empty() && !is_numeric_literal(source) &&
            (resolved != source || identity_rule)) {
            put_ordered(&result.unique_entries, &result.unique_order, source, resolved);
            if (translations.size() > 1 && !unchanged) ++result.dominant_entry_count;
        } else if (resolved.empty()) {
            ++result.conflict_count;
        }
        begin = end;
    }

    struct TermCandidate {
        std::string source;
        std::string translation;
        int priority = 0;
    };
    std::vector<TermCandidate> term_candidates;
    auto add_term = [&](const std::string &source, const std::string &translation, int priority,
                        bool phrases) {
        const std::string normalized_source = strip_paired_brackets(source);
        const std::string normalized_translation = strip_paired_brackets(translation);
        if (is_numeric_literal(normalized_source) ||
            !is_term_like(normalized_source, 2, phrases) ||
            !is_term_like(normalized_translation, 1) ||
            normalized_source == normalized_translation) {
            return;
        }
        term_candidates.push_back({normalized_source, normalized_translation, priority});
    };
    for (const IndexRecord *record : usable) {
        if (record->source == record->translation) continue;
        const int priority = term_priority(record->path);
        if (is_term_field(record->field)) {
            const bool phrases = record->field == "name" || record->field == "shortName" ||
                                 record->field == "abName" || record->field == "title" ||
                                 priority >= 2;
            add_term(record->source, record->translation, priority, phrases);
            add_term(strip_decorative_tags(record->source),
                     strip_decorative_tags(record->translation), priority, phrases);
            const std::vector<std::string> source_terms = bracket_contents(record->source);
            const std::vector<std::string> translation_terms = bracket_contents(record->translation);
            if (source_terms.size() == translation_terms.size()) {
                for (size_t i = 0; i < source_terms.size(); ++i) {
                    add_term(source_terms[i], translation_terms[i], priority, true);
                }
            }
        }
        if (is_structured_description_field(record->field)) {
            const std::string source_text = strip_decorative_tags(record->source);
            const std::string translation_text = strip_decorative_tags(record->translation);
            size_t source_at = 0;
            size_t translation_at = 0;
            while (source_at <= source_text.size() && translation_at <= translation_text.size()) {
                const size_t source_end = source_text.find('\n', source_at);
                const size_t translation_end = translation_text.find('\n', translation_at);
                const std::string source_line =
                    source_text.substr(source_at, source_end == std::string::npos
                                                     ? std::string::npos
                                                     : source_end - source_at);
                const std::string translation_line =
                    translation_text.substr(translation_at, translation_end == std::string::npos
                                                                  ? std::string::npos
                                                                  : translation_end - translation_at);
                const std::string source_clause = leading_clause(source_line);
                const std::string translation_clause = leading_clause(translation_line);
                if (!source_clause.empty() && !translation_clause.empty() &&
                    source_names.find(source_clause) == source_names.end()) {
                    add_term(source_clause, translation_clause, 0, true);
                }
                if (source_end == std::string::npos || translation_end == std::string::npos) break;
                source_at = source_end + 1;
                translation_at = translation_end + 1;
            }
        }
    }
    std::map<std::string, std::vector<TermCandidate>> candidates_by_source;
    for (const TermCandidate &candidate : term_candidates) {
        candidates_by_source[candidate.source].push_back(candidate);
    }
    for (const auto &group : candidates_by_source) {
        if (unchanged_sources.find(group.first) != unchanged_sources.end()) continue;
        int highest = 0;
        for (const TermCandidate &candidate : group.second) {
            highest = std::max(highest, candidate.priority);
        }
        std::map<std::string, uint32_t> counts;
        for (const TermCandidate &candidate : group.second) {
            if (candidate.priority == highest) ++counts[candidate.translation];
        }
        const std::string resolved = resolve_translation(counts);
        if (resolved.empty()) continue;
        put_ordered(&result.term_entries, &result.term_order, group.first, resolved);
        if (highest == 3) {
            put_ordered(&result.unique_entries, &result.unique_order, group.first, resolved);
        }
    }

    begin = 0;
    while (begin < usable.size()) {
        size_t end = begin + 1;
        while (end < usable.size() && usable[end]->source == usable[begin]->source) ++end;
        const std::string &source = usable[begin]->source;
        std::set<std::string> distinct;
        for (size_t i = begin; i < end; ++i) distinct.insert(usable[i]->translation);
        const auto global = result.unique_entries.find(source);
        bool needs_context = distinct.size() > 1 || is_numeric_literal(source) ||
                             translated_outputs.find(source) != translated_outputs.end();
        if (!needs_context && global != result.unique_entries.end()) {
            for (size_t i = begin; i < end; ++i) {
                if (usable[i]->translation != global->second) needs_context = true;
            }
        }
        if (needs_context) {
            std::map<std::string, std::vector<std::string>> targets;
            for (size_t i = begin; i < end; ++i) {
                for (const std::string &id : record_ids(*usable[i])) {
                    const std::string key = context_key(id, usable[i]->field, source);
                    if (!key.empty()) targets[key].push_back(usable[i]->translation);
                }
            }
            for (const auto &target : targets) {
                std::set<std::string> values(target.second.begin(), target.second.end());
                std::string chosen;
                if (values.size() == 1) {
                    chosen = *values.begin();
                } else if (values.find(source) != values.end()) {
                    chosen = source;
                }
                if (!chosen.empty()) {
                    put_ordered(&result.contextual_entries, &result.contextual_order, target.first,
                                chosen);
                }
            }
        }
        begin = end;
    }

    shared::Sha256 digest;
    auto update = [&](const std::string &value) {
        const uint32_t length = static_cast<uint32_t>(value.size());
        const unsigned char header[4] = {
            static_cast<unsigned char>(length >> 24),
            static_cast<unsigned char>(length >> 16),
            static_cast<unsigned char>(length >> 8),
            static_cast<unsigned char>(length)};
        digest.update(header, sizeof(header));
        digest.update(reinterpret_cast<const uint8_t *>(value.data()), value.size());
    };
    update(kRevision);
    // TranslationIndexCompiler.kt:244 digests path, id, field, source, translation,
    // stableId in that order (IndexRecord::context carries the reference `id`).
    for (const IndexRecord *record : usable) {
        update(record->path);
        update(record->context);
        update(record->field);
        update(record->source);
        update(record->translation);
        update(record->stable_id);
        if (record->source != record->translation) ++result.source_record_count;
    }
    uint8_t hash[32];
    digest.finish(hash);
    static const char digits[] = "0123456789abcdef";
    result.input_sha256.reserve(64);
    for (const uint8_t byte : hash) {
        result.input_sha256.push_back(digits[byte >> 4]);
        result.input_sha256.push_back(digits[byte & 15]);
    }
    return result;
}

bool write_translation_index(const CompiledTranslationIndex &index, std::string *out,
                             std::string *error) {
    if (!out || !error) return false;
    if (index.unique_entries.size() > kMaxEntries || index.term_entries.size() > kMaxEntries ||
        index.contextual_entries.size() > kMaxEntries) {
        *error = "index_count";
        return false;
    }
    auto check = [error](const std::map<std::string, std::string> &entries) {
        for (const auto &entry : entries) {
            if (entry.first.empty() || entry.first.size() > kMaxStringBytes ||
                entry.second.empty() || entry.second.size() > kMaxStringBytes) {
                *error = "index_entry";
                return false;
            }
        }
        return true;
    };
    if (!check(index.unique_entries) || !check(index.term_entries) ||
        !check(index.contextual_entries)) {
        return false;
    }
    // TranslationIndexCompiler.kt:538-556 writes magic, schema and the three
    // counts first, then one payload per group; there is no per-group count.
    std::string payload;
    const uint32_t unique_count = append_pairs(&payload, index.unique_entries, index.unique_order);
    const uint32_t term_count = append_pairs(&payload, index.term_entries, index.term_order);
    const uint32_t contextual_count =
        append_pairs(&payload, index.contextual_entries, index.contextual_order);
    std::string bytes;
    bytes.reserve(sizeof(kMagic) + 4 * 4 + payload.size());
    bytes.append(reinterpret_cast<const char *>(kMagic), sizeof(kMagic));
    append_u32(&bytes, kIndexSchema);
    append_u32(&bytes, unique_count);
    append_u32(&bytes, term_count);
    append_u32(&bytes, contextual_count);
    bytes.append(payload);
    if (bytes.size() > kMaxIndexBytes) {
        *error = "index_size";
        return false;
    }
    *out = std::move(bytes);
    error->clear();
    return true;
}

} // namespace bybus::translation
