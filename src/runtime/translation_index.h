// Runtime reader for the schema-9 translation index produced by bybusd.
// The file is immutable for one generation and is parsed before any text hook
// is installed; lookups never touch disk.
#pragma once

#include <cstddef>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace bybus::runtime {

class TranslationIndex {
public:
    bool load(const std::string &bytes, std::string *error);
    bool empty() const { return unique_.empty() && terms_.empty() && contextual_.empty(); }

    const std::string *find(const std::string &source) const;
    const std::string *find_context(const std::string &key) const;
    const std::string *find_term(const std::string &source) const;

    size_t unique_count() const { return unique_.size(); }
    size_t term_count() const { return terms_.size(); }
    size_t contextual_count() const { return contextual_.size(); }

private:
    std::unordered_map<std::string, std::string> unique_;
    std::unordered_map<std::string, std::string> terms_;
    std::unordered_map<std::string, std::string> contextual_;
};

// id + unit separator + field + unit separator + source
std::string translation_context_key(const std::string &id, const std::string &field,
                                    const std::string &source);

} // namespace bybus::runtime
