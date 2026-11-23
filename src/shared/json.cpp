#include "shared/json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bybus::shared {
namespace {

constexpr int kMaxDepth = 512;

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void append_utf8(uint32_t codepoint, std::string *out) {
    if (codepoint < 0x80) {
        out->push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

// Validates one UTF-8 sequence starting at text[i]; returns its byte length or 0.
size_t utf8_sequence(const std::string &text, size_t i) {
    const unsigned char first = static_cast<unsigned char>(text[i]);
    size_t length = 0;
    uint32_t minimum = 0;
    uint32_t codepoint = 0;
    if (first < 0x80) return 1;
    if ((first & 0xE0) == 0xC0) { length = 2; codepoint = first & 0x1F; minimum = 0x80; }
    else if ((first & 0xF0) == 0xE0) { length = 3; codepoint = first & 0x0F; minimum = 0x800; }
    else if ((first & 0xF8) == 0xF0) { length = 4; codepoint = first & 0x07; minimum = 0x10000; }
    else return 0;
    if (i + length > text.size()) return 0;
    for (size_t k = 1; k < length; ++k) {
        const unsigned char next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xC0) != 0x80) return 0;
        codepoint = (codepoint << 6) | (next & 0x3F);
    }
    if (codepoint < minimum || codepoint > 0x10FFFF) return 0;
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return 0;
    return length;
}

class Parser {
public:
    explicit Parser(const std::string &text) : text_(text) {}

    ParseOutcome run() {
        ParseOutcome outcome;
        if (text_.compare(0, 3, "\xEF\xBB\xBF") == 0) position_ = 3;
        skip_space();
        JsonPtr value = parse_value(0);
        if (!ok_) {
            outcome.error = error_;
            return outcome;
        }
        skip_space();
        if (position_ != text_.size()) {
            outcome.error = "json_trailing_data";
            return outcome;
        }
        outcome.value = value;
        outcome.ok = true;
        return outcome;
    }

private:
    void skip_space() {
        while (position_ < text_.size() && is_space(text_[position_])) ++position_;
    }

    bool fail(const char *error) {
        if (ok_) {
            ok_ = false;
            error_ = error;
        }
        return false;
    }

    JsonPtr parse_value(int depth) {
        if (depth > kMaxDepth) {
            fail("json_depth");
            return nullptr;
        }
        if (position_ >= text_.size()) {
            fail("json_unexpected_end");
            return nullptr;
        }
        const char c = text_[position_];
        if (c == '{') return parse_object(depth);
        if (c == '[') return parse_array(depth);
        if (c == '"' || c == '\'') return parse_string();
        if (c == '-' || is_digit(c)) return parse_number();
        return parse_literal();
    }

    JsonPtr parse_object(int depth) {
        JsonPtr result = Json::object();
        ++position_;  // '{'
        skip_space();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return result;
        }
        while (true) {
            skip_space();
            if (position_ >= text_.size()) {
                fail("json_unexpected_end");
                return nullptr;
            }
            if (text_[position_] != '"' && text_[position_] != '\'') {
                fail("json_object_key");
                return nullptr;
            }
            JsonPtr key = parse_string();
            if (!key) return nullptr;
            skip_space();
            if (position_ >= text_.size() || text_[position_] != ':') {
                fail("json_object_colon");
                return nullptr;
            }
            ++position_;
            skip_space();
            JsonPtr value = parse_value(depth + 1);
            if (!value) return nullptr;
            result->put(key->as_string(), value);
            skip_space();
            if (position_ >= text_.size()) {
                fail("json_unexpected_end");
                return nullptr;
            }
            if (text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (text_[position_] == '}') {
                ++position_;
                return result;
            }
            fail("json_object_separator");
            return nullptr;
        }
    }

    JsonPtr parse_array(int depth) {
        JsonPtr result = Json::array();
        ++position_;  // '['
        skip_space();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return result;
        }
        while (true) {
            skip_space();
            JsonPtr value = parse_value(depth + 1);
            if (!value) return nullptr;
            result->append(value);
            skip_space();
            if (position_ >= text_.size()) {
                fail("json_unexpected_end");
                return nullptr;
            }
            if (text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (text_[position_] == ']') {
                ++position_;
                return result;
            }
            fail("json_array_separator");
            return nullptr;
        }
    }

    JsonPtr parse_string() {
        const char quote = text_[position_++];
        std::string out;
        while (true) {
            if (position_ >= text_.size()) {
                fail("json_string_end");
                return nullptr;
            }
            const char c = text_[position_];
            if (c == quote) {
                ++position_;
                return Json::string(out);
            }
            if (c == '\\') {
                ++position_;
                if (position_ >= text_.size()) {
                    fail("json_escape_end");
                    return nullptr;
                }
                const char escape = text_[position_++];
                switch (escape) {
                    case '"': out.push_back('"'); break;
                    case '\'': out.push_back('\''); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t codepoint = 0;
                        if (!read_hex4(&codepoint)) return nullptr;
                        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                            if (position_ + 1 >= text_.size() || text_[position_] != '\\' ||
                                text_[position_ + 1] != 'u') {
                                fail("json_lone_surrogate");
                                return nullptr;
                            }
                            position_ += 2;
                            uint32_t low = 0;
                            if (!read_hex4(&low)) return nullptr;
                            if (low < 0xDC00 || low > 0xDFFF) {
                                fail("json_lone_surrogate");
                                return nullptr;
                            }
                            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                            fail("json_lone_surrogate");
                            return nullptr;
                        }
                        append_utf8(codepoint, &out);
                        break;
                    }
                    default:
                        fail("json_escape");
                        return nullptr;
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("json_control_character");
                return nullptr;
            }
            const size_t length = utf8_sequence(text_, position_);
            if (length == 0) {
                fail("json_invalid_utf8");
                return nullptr;
            }
            out.append(text_, position_, length);
            position_ += length;
        }
    }

    bool read_hex4(uint32_t *value) {
        if (position_ + 4 > text_.size()) {
            fail("json_unicode_escape");
            return false;
        }
        uint32_t result = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = hex_value(text_[position_ + static_cast<size_t>(i)]);
            if (digit < 0) {
                fail("json_unicode_escape");
                return false;
            }
            result = (result << 4) | static_cast<uint32_t>(digit);
        }
        position_ += 4;
        *value = result;
        return true;
    }

    JsonPtr parse_number() {
        const size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        if (position_ + 1 < text_.size() && text_[position_] == '0' &&
            (text_[position_ + 1] == 'x' || text_[position_ + 1] == 'X')) {
            position_ += 2;
            const size_t digits_start = position_;
            int64_t value = 0;
            while (position_ < text_.size() && hex_value(text_[position_]) >= 0) {
                value = value * 16 + hex_value(text_[position_]);
                ++position_;
            }
            if (position_ == digits_start) {
                fail("json_number");
                return nullptr;
            }
            return Json::integer(value);
        }
        bool integral = true;
        while (position_ < text_.size() && is_digit(text_[position_])) ++position_;
        if (position_ < text_.size() && text_[position_] == '.') {
            integral = false;
            ++position_;
            while (position_ < text_.size() && is_digit(text_[position_])) ++position_;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            integral = false;
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            const size_t digits_start = position_;
            while (position_ < text_.size() && is_digit(text_[position_])) ++position_;
            if (position_ == digits_start) {
                fail("json_number_exponent");
                return nullptr;
            }
        }
        const std::string token = text_.substr(start, position_ - start);
        if (token.empty() || token == "-") {
            fail("json_number");
            return nullptr;
        }
        if (integral) {
            errno = 0;
            const long long value = std::strtoll(token.c_str(), nullptr, 10);
            if (errno == 0) {
                if (value >= INT32_MIN && value <= INT32_MAX) {
                    return Json::integer(static_cast<int32_t>(value));
                }
                return Json::integer(value);
            }
        }
        return Json::number(std::strtod(token.c_str(), nullptr));
    }

    JsonPtr parse_literal() {
        if (text_.compare(position_, 4, "true") == 0) {
            position_ += 4;
            return Json::boolean(true);
        }
        if (text_.compare(position_, 5, "false") == 0) {
            position_ += 5;
            return Json::boolean(false);
        }
        if (text_.compare(position_, 4, "null") == 0) {
            position_ += 4;
            return Json::null();
        }
        fail("json_unexpected_token");
        return nullptr;
    }

    const std::string &text_;
    size_t position_ = 0;
    bool ok_ = true;
    std::string error_ = "json_invalid";
};

// Java Double.toString is shortest-round-trip. This approximation keeps the
// decimal point and exponent form Java uses; tables rarely carry doubles and
// the value is never re-parsed for pairing decisions.
std::string double_text(double value) {
    if (std::isnan(value) || std::isinf(value)) return "null";
    char buffer[64];
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value) break;
    }
    std::string text(buffer);
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find('E') == std::string::npos) {
        text += ".0";
    }
    return text;
}

void quote_string(const std::string &value, std::string *out) {
    out->push_back('"');
    for (const char c : value) {
        switch (c) {
            case '"': out->append("\\\""); break;
            case '\\': out->append("\\\\"); break;
            case '/': out->append("\\/"); break;
            case '\b': out->append("\\b"); break;
            case '\f': out->append("\\f"); break;
            case '\n': out->append("\\n"); break;
            case '\r': out->append("\\r"); break;
            case '\t': out->append("\\t"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                    out->append(buffer);
                } else {
                    out->push_back(c);
                }
                break;
        }
    }
    out->push_back('"');
}

} // namespace

Json::Json() : type_(Type::Null) {}

Json::Json(Type type) : type_(type) {}

JsonPtr Json::object() { return std::make_shared<Json>(Type::Object); }
JsonPtr Json::array() { return std::make_shared<Json>(Type::Array); }

JsonPtr Json::string(const std::string &value) {
    auto result = std::make_shared<Json>(Type::String);
    result->text_ = value;
    return result;
}

JsonPtr Json::boolean(bool value) {
    auto result = std::make_shared<Json>(Type::Bool);
    result->bool_ = value;
    return result;
}

JsonPtr Json::integer(int64_t value) {
    auto result = std::make_shared<Json>(
        value >= INT32_MIN && value <= INT32_MAX ? Type::Int : Type::Long);
    result->integer_ = value;
    result->double_ = static_cast<double>(value);
    return result;
}

JsonPtr Json::number(double value) {
    auto result = std::make_shared<Json>(Type::Double);
    result->double_ = value;
    return result;
}

JsonPtr Json::null() { return std::make_shared<Json>(Type::Null); }

int64_t Json::as_int() const {
    if (type_ == Type::Int || type_ == Type::Long) return integer_;
    if (type_ == Type::Double) return static_cast<int64_t>(double_);
    return 0;
}

double Json::as_double() const {
    if (type_ == Type::Double) return double_;
    if (type_ == Type::Int || type_ == Type::Long) return static_cast<double>(integer_);
    return 0.0;
}

size_t Json::length() const {
    if (type_ == Type::Array) return items_.size();
    if (type_ == Type::Object) return members_.size();
    return 0;
}

JsonPtr Json::at(size_t index) const {
    if (type_ != Type::Array || index >= items_.size()) return nullptr;
    return items_[index];
}

void Json::set_at(size_t index, JsonPtr value) {
    if (type_ != Type::Array || index >= items_.size()) return;
    items_[index] = std::move(value);
}

void Json::append(JsonPtr value) {
    if (type_ != Type::Array) return;
    items_.push_back(std::move(value));
}

void Json::reindex() const {
    index_.clear();
    index_.reserve(members_.size());
    for (size_t i = 0; i < members_.size(); ++i) index_.emplace_back(members_[i].first, i);
    index_valid_ = true;
}

size_t Json::find_member(const std::string &field) const {
    if (!index_valid_) reindex();
    for (const auto &entry : index_) {
        if (entry.first == field) return entry.second;
    }
    return static_cast<size_t>(-1);
}

JsonPtr Json::opt(const std::string &field) const {
    if (type_ != Type::Object) return nullptr;
    const size_t at = find_member(field);
    if (at == static_cast<size_t>(-1)) return nullptr;
    return members_[at].second;
}

bool Json::has(const std::string &field) const {
    return type_ == Type::Object && find_member(field) != static_cast<size_t>(-1);
}

void Json::put(const std::string &field, JsonPtr value) {
    if (type_ != Type::Object) return;
    const size_t at = find_member(field);
    if (at != static_cast<size_t>(-1)) {
        members_[at].second = std::move(value);
        return;
    }
    members_.emplace_back(field, std::move(value));
    if (index_valid_) index_.emplace_back(field, members_.size() - 1);
}

void Json::put_string(const std::string &field, const std::string &value) {
    put(field, Json::string(value));
}

void Json::put_int(const std::string &field, int64_t value) { put(field, Json::integer(value)); }

void Json::put_bool(const std::string &field, bool value) { put(field, Json::boolean(value)); }

void Json::put_null(const std::string &field) { put(field, Json::null()); }

JsonPtr Json::put_object(const std::string &field) {
    JsonPtr value = Json::object();
    put(field, value);
    return value;
}

JsonPtr Json::put_array(const std::string &field) {
    JsonPtr value = Json::array();
    put(field, value);
    return value;
}

void Json::remove(const std::string &field) {
    if (type_ != Type::Object) return;
    for (auto it = members_.begin(); it != members_.end(); ++it) {
        if (it->first == field) {
            members_.erase(it);
            index_valid_ = false;
            return;
        }
    }
}

std::vector<std::string> Json::keys() const {
    std::vector<std::string> result;
    if (type_ != Type::Object) return result;
    result.reserve(members_.size());
    for (const auto &entry : members_) result.push_back(entry.first);
    return result;
}

std::string Json::dump() const {
    std::string out;
    dump_to(&out);
    return out;
}

void Json::dump_to(std::string *out) const {
    switch (type_) {
        case Type::Null: out->append("null"); break;
        case Type::Bool: out->append(bool_ ? "true" : "false"); break;
        case Type::Int:
        case Type::Long: {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(integer_));
            out->append(buffer);
            break;
        }
        case Type::Double: out->append(double_text(double_)); break;
        case Type::String: quote_string(text_, out); break;
        case Type::Array: {
            out->push_back('[');
            for (size_t i = 0; i < items_.size(); ++i) {
                if (i != 0) out->push_back(',');
                if (items_[i]) items_[i]->dump_to(out);
                else out->append("null");
            }
            out->push_back(']');
            break;
        }
        case Type::Object: {
            out->push_back('{');
            for (size_t i = 0; i < members_.size(); ++i) {
                if (i != 0) out->push_back(',');
                quote_string(members_[i].first, out);
                out->push_back(':');
                if (members_[i].second) members_[i].second->dump_to(out);
                else out->append("null");
            }
            out->push_back('}');
            break;
        }
    }
}

JsonPtr Json::clone() const {
    auto copy = std::make_shared<Json>(type_);
    copy->bool_ = bool_;
    copy->integer_ = integer_;
    copy->double_ = double_;
    copy->text_ = text_;
    copy->items_.reserve(items_.size());
    for (const auto &item : items_) copy->items_.push_back(item ? item->clone() : nullptr);
    copy->members_.reserve(members_.size());
    for (const auto &entry : members_) {
        copy->members_.emplace_back(entry.first, entry.second ? entry.second->clone() : nullptr);
    }
    return copy;
}

ParseOutcome parse(const std::string &text) {
    Parser parser(text);
    return parser.run();
}

namespace {

bool equivalent_impl(const JsonPtr &left, const JsonPtr &right, int depth) {
    if (depth > 64) return false;
    if (!left || !right) return !left && !right;
    if (left->type() != right->type()) return false;
    switch (left->type()) {
        case Json::Type::Null: return true;
        case Json::Type::Bool: return left->as_bool() == right->as_bool();
        case Json::Type::Int:
        case Json::Type::Long: return left->as_int() == right->as_int();
        case Json::Type::Double: return left->as_double() == right->as_double();
        case Json::Type::String: return left->as_string() == right->as_string();
        case Json::Type::Array: {
            if (left->length() != right->length()) return false;
            for (size_t i = 0; i < left->length(); ++i) {
                if (!equivalent_impl(left->at(i), right->at(i), depth + 1)) return false;
            }
            return true;
        }
        case Json::Type::Object: {
            const auto left_keys = left->keys();
            const auto right_keys = right->keys();
            if (left_keys.size() != right_keys.size()) return false;
            for (const auto &key : left_keys) {
                if (!right->has(key)) return false;
                if (!equivalent_impl(left->opt(key), right->opt(key), depth + 1)) return false;
            }
            return true;
        }
    }
    return false;
}

} // namespace

bool equivalent(const JsonPtr &left, const JsonPtr &right) {
    return equivalent_impl(left, right, 0);
}

} // namespace bybus::shared
