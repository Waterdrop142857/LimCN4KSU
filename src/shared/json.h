// Minimal JSON model matching the semantics the Kotlin pairer relies on:
// - objects keep insertion order, duplicate keys overwrite in place
// - numbers keep Integer / Long / Double identity, because equivalentJson()
//   compares boxed values strictly while snapshot binding compares decimals
// - parse() consumes exactly one value and rejects trailing data
// - dump() follows Android org.json escaping ('"', '\\', '/', controls)
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bybus::shared {

class Json;
using JsonPtr = std::shared_ptr<Json>;

class Json {
public:
    enum class Type { Null, Bool, Int, Long, Double, String, Array, Object };

    Json();
    explicit Json(Type type);

    static JsonPtr object();
    static JsonPtr array();
    static JsonPtr string(const std::string &value);
    static JsonPtr boolean(bool value);
    static JsonPtr integer(int64_t value);          // Int when int32 range, else Long
    static JsonPtr number(double value);
    static JsonPtr null();

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_string() const { return type_ == Type::String; }
    bool is_number() const {
        return type_ == Type::Int || type_ == Type::Long || type_ == Type::Double;
    }
    bool is_integral() const { return type_ == Type::Int || type_ == Type::Long; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }

    // Only meaningful for the matching type; other types read as empty/zero.
    const std::string &as_string() const { return text_; }
    bool as_bool() const { return bool_; }
    int64_t as_int() const;
    double as_double() const;

    size_t length() const;
    const std::vector<JsonPtr> &items() const { return items_; }
    const std::vector<std::pair<std::string, JsonPtr>> &members() const { return members_; }

    JsonPtr at(size_t index) const;
    void set_at(size_t index, JsonPtr value);
    void append(JsonPtr value);

    // Returns nullptr when absent or when this value is not an object.
    JsonPtr opt(const std::string &field) const;
    bool has(const std::string &field) const;
    void put(const std::string &field, JsonPtr value);
    void put_string(const std::string &field, const std::string &value);
    void put_int(const std::string &field, int64_t value);
    void put_bool(const std::string &field, bool value);
    void put_null(const std::string &field);
    JsonPtr put_object(const std::string &field);
    JsonPtr put_array(const std::string &field);
    void remove(const std::string &field);
    std::vector<std::string> keys() const;

    std::string dump() const;
    void dump_to(std::string *out) const;

    // Deep copy; object order and number identity are preserved.
    JsonPtr clone() const;

private:
    Type type_;
    bool bool_ = false;
    int64_t integer_ = 0;
    double double_ = 0.0;
    std::string text_;
    std::vector<JsonPtr> items_;
    std::vector<std::pair<std::string, JsonPtr>> members_;
    // Parallel index of members_ by field name, rebuilt lazily after removals.
    mutable std::vector<std::pair<std::string, size_t>> index_;
    mutable bool index_valid_ = false;

    size_t find_member(const std::string &field) const;
    void reindex() const;
};

struct ParseOutcome {
    JsonPtr value;
    bool ok = false;
    std::string error;
};

// Single value parse. Leading BOM is skipped (the caller may also strip it).
// Returns ok=false with a short machine-readable error on any violation.
ParseOutcome parse(const std::string &text);

// Strict structural equality: number identity must match exactly. Mirrors
// TranslationJsonPairer.equivalentJson.
bool equivalent(const JsonPtr &left, const JsonPtr &right);

} // namespace bybus::shared
