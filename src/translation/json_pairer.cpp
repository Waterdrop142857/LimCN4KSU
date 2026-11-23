#include "translation/json_pairer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "shared/sha256.h"
#include "translation/text_policy.h"

namespace bybus::translation {
namespace {

using shared::Json;
using shared::JsonPtr;

constexpr int kMaxDepth = 64;

const char *const kIdFields[] = {"id", "key", "level", "index", "idx", "originalId"};
const char *const kStableMetadataFields[] = {"model", "voicefile"};
const char *const kSkillChildFields[] = {"levellist", "coinlist", "coindescs"};

bool is_blank(const std::string &text) {
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    }
    return true;
}

bool is_skill_number(const std::string &text) {
    if (text.empty()) return false;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

bool starts_with(const std::string &text, const char *prefix) {
    const std::string head(prefix);
    return text.size() >= head.size() && text.compare(0, head.size(), head) == 0;
}

bool is_ascii_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool is_scope_identifier(const std::string &text, bool allow_dot) {
    if (text.empty()) return false;
    for (const char c : text) {
        const bool extra = c == '-' || c == '_' || (allow_dot && c == '.');
        if (!is_ascii_alnum(c) && !extra) return false;
    }
    return true;
}

std::string lower_ascii(std::string text) {
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

// Mirrors `value is String || value is Number` then toString().
bool id_text(const JsonPtr &value, std::string *out) {
    if (!value) return false;
    if (value->is_string()) {
        *out = value->as_string();
        return true;
    }
    if (value->is_number()) {
        *out = value->dump();
        return true;
    }
    return false;
}

std::string substring_before_last(const std::string &text, char separator) {
    const size_t at = text.rfind(separator);
    return at == std::string::npos ? std::string() : text.substr(0, at);
}

// sourceTableName(): strips the language prefix of a resource file name.
std::string table_name(const std::string &path) {
    const size_t at = path.rfind('/');
    std::string name = at == std::string::npos ? path : path.substr(at + 1);
    if (name.size() > 3) {
        const std::string prefix = name.substr(0, 3);
        if (prefix == "JP_" || prefix == "EN_" || prefix == "KR_") name = name.substr(3);
    }
    return name;
}

bool belongs_to(const std::string &path, const char *family) {
    const std::string file = table_name(path);
    const std::string base(family);
    if (file == base + ".json") return true;
    if (file.size() > base.size() + 1 && file.compare(0, base.size(), base) == 0) {
        return file[base.size()] == '_' || file[base.size()] == '-';
    }
    return false;
}

bool is_skill_child_field(const std::string &field) {
    for (const char *candidate : kSkillChildFields) {
        if (field == candidate) return true;
    }
    return false;
}

std::string identity_of(const JsonPtr &object) {
    if (!object || !object->is_object()) return std::string();
    for (const char *field : kIdFields) {
        std::string text;
        if (id_text(object->opt(field), &text)) return std::string(field) + "=" + text;
    }
    return std::string();
}

std::string direct_id(const JsonPtr &object) {
    static const char *const kFields[] = {"id", "key"};
    for (const char *field : kFields) {
        std::string text;
        if (id_text(object->opt(field), &text)) return text;
    }
    return std::string();
}

std::string table_identity(const std::string &path, const std::string &id) {
    if (belongs_to(path, "Bufs")) return "@buf:" + id;
    if (belongs_to(path, "BattleKeywords")) return "@battle-keyword:" + id;
    if (belongs_to(path, "SkillTag")) return "@skill-tag:" + id;
    if (belongs_to(path, "EgoGiftCategory")) return "@gift-category:" + id;
    return id;
}

std::vector<std::pair<std::string, std::string>> stable_metadata(const JsonPtr &object) {
    std::vector<std::pair<std::string, std::string>> result;
    for (const char *field : kStableMetadataFields) {
        const JsonPtr value = object->opt(field);
        if (value && value->is_string() && !is_blank(value->as_string())) {
            result.emplace_back(field, value->as_string());
        }
    }
    return result;
}

std::vector<std::string> display_shape(const JsonPtr &object) {
    std::vector<std::string> result;
    for (const auto &member : object->members()) {
        const JsonPtr value = member.second;
        if (is_display_field(member.first) && value && value->is_string()) {
            result.push_back(member.first);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

struct DuplicateIdentity {
    bool valid = false;
    std::vector<std::pair<std::string, std::string>> metadata;
    std::vector<std::string> shape;
};

DuplicateIdentity duplicate_identity(const JsonPtr &object, bool story) {
    DuplicateIdentity result;
    result.metadata = stable_metadata(object);
    if (!story && result.metadata.empty()) return result;
    if (story) result.shape = display_shape(object);
    result.valid = true;
    return result;
}

bool same_duplicate(const DuplicateIdentity &left, const DuplicateIdentity &right) {
    if (left.valid != right.valid) return false;
    if (!left.valid) return true;
    return left.metadata == right.metadata && left.shape == right.shape;
}

JsonPtr non_display_metadata(const JsonPtr &row) {
    JsonPtr result = Json::object();
    for (const auto &member : row->members()) {
        if (is_display_field(member.first)) continue;
        result->put(member.first, member.second);
    }
    return result;
}

int count_fields(const JsonPtr &value, int depth, const std::string *display_array) {
    if (depth > kMaxDepth) return -1;
    if (!value) return 0;
    if (value->is_object()) {
        int total = 0;
        for (const auto &member : value->members()) {
            const JsonPtr child = member.second;
            // The scope must follow the field being inspected, exactly like the
            // Kotlin recursion that passes `field.takeIf { it in STRING_ARRAY_FIELDS }`.
            if (child && child->is_string() && is_display_field(member.first)) {
                ++total;
                continue;
            }
            const bool is_array_field = is_string_array_field(member.first);
            const std::string *nested = is_array_field ? &member.first : nullptr;
            const int nested_count = count_fields(child, depth + 1, nested);
            if (nested_count < 0) return -1;
            total += nested_count;
        }
        return total;
    }
    if (value->is_array()) {
        int total = 0;
        for (const JsonPtr &item : value->items()) {
            const int nested_count = count_fields(item, depth + 1, display_array);
            if (nested_count < 0) return -1;
            total += nested_count;
        }
        return total;
    }
    if (value->is_string()) {
        return display_array != nullptr ? 1 : 0;
    }
    return 0;
}

bool is_explicitly_empty(const JsonPtr &value) {
    if (!value) return false;
    if (value->is_array()) return value->length() == 0;
    if (value->is_object()) {
        const JsonPtr data_list = value->opt("dataList");
        return data_list && data_list->is_array() && data_list->length() == 0;
    }
    return false;
}

struct Scope {
    const std::string *display_array = nullptr;
    const std::string *array_owner = nullptr;
    const std::string *skill_context = nullptr;
    const std::string *skill_relation = nullptr;
    const std::string *object_scope = nullptr;
};

class Collector {
public:
    Collector(std::string path, std::vector<IndexRecord> *records,
              const SnapshotBinding *snapshots, std::vector<std::function<void()>> *edits)
        : path_(std::move(path)), records_(records), snapshots_(snapshots), edits_(edits) {}

    bool collect(const JsonPtr &original, const JsonPtr &translated, const std::string &context,
                 int depth, const Scope &scope, std::string *error);

private:
    bool collect_object(const JsonPtr &original, const JsonPtr &translated,
                        const std::string &context, int depth, const Scope &scope,
                        std::string *error);
    bool collect_array(const JsonPtr &original, const JsonPtr &translated,
                       const std::string &context, int depth, const Scope &scope,
                       std::string *error);
    bool collect_gift_abilities(const JsonPtr &original, const JsonPtr &translated,
                                const std::string &context, int depth,
                                const std::string &parent, std::string *error);
    bool collect_table_children(const JsonPtr &original, const JsonPtr &translated,
                                const std::string &context, int depth, const std::string &parent,
                                const char *identity_field, std::string *error);

    JsonPtr snapshot_target(const JsonPtr &source) const;

    std::string path_;
    std::vector<IndexRecord> *records_;
    const SnapshotBinding *snapshots_;
    std::vector<std::function<void()>> *edits_;
};

JsonPtr Collector::snapshot_target(const JsonPtr &source) const {
    if (!snapshots_) return nullptr;
    const auto found = snapshots_->find(source.get());
    return found == snapshots_->end() ? nullptr : found->second;
}

bool Collector::collect(const JsonPtr &original, const JsonPtr &translated,
                        const std::string &context, int depth, const Scope &scope,
                        std::string *error) {
    if (depth > kMaxDepth) {
        *error = "pairer_nesting_limit";
        return false;
    }
    if (!original) return true;
    if (original->is_object() && translated && translated->is_object()) {
        return collect_object(original, translated, context, depth, scope, error);
    }
    if (original->is_array() && translated && translated->is_array()) {
        return collect_array(original, translated, context, depth, scope, error);
    }
    return true;
}

bool Collector::collect_object(const JsonPtr &original, const JsonPtr &translated,
                               const std::string &context, int depth, const Scope &scope,
                               std::string *error) {
    const std::string identity = identity_of(original);
    if (!identity.empty() && identity != identity_of(translated)) return true;
    const std::string object_context =
        identity.empty() ? context : context + "[" + identity + "]";
    const std::string id = direct_id(original);
    const std::string file = table_name(path_);
    std::string skill_root;
    if (scope.skill_context) {
        skill_root = *scope.skill_context;
    } else if (!id.empty() && is_skill_number(id) && original->opt("levelList") &&
               original->opt("levelList")->is_array() &&
               (file == "Skills.json" || file.compare(0, 7, "Skills_") == 0)) {
        skill_root = "@skill:" + id;
    }
    std::string table_scope;
    if (scope.object_scope) {
        table_scope = *scope.object_scope;
    } else if (!id.empty()) {
        if (is_skill_number(id) && belongs_to(path_, "EGOgift")) {
            table_scope = "@ego-gift:" + id;
        } else if (is_skill_number(id) && belongs_to(path_, "ActionEvents")) {
            table_scope = "@action-event:" + id;
        } else if (is_skill_number(id) && belongs_to(path_, "AbnormalityGuides")) {
            table_scope = "@abnormality-guide:" + id;
        } else if (is_scope_identifier(id, true) && belongs_to(path_, "rpg-loc-dialogue")) {
            table_scope = "@rpg-dialogue:" + id;
        }
    }
    std::string story_scope;
    const std::string parent_folder = substring_before_last(path_, '/');
    if (parent_folder.size() >= 10 &&
        parent_folder.compare(parent_folder.size() - 10, 10, "/StoryData") == 0) {
        std::string scene = table_name(path_);
        if (scene.size() > 5 && scene.compare(scene.size() - 5, 5, ".json") == 0) {
            scene = scene.substr(0, scene.size() - 5);
        }
        if (is_scope_identifier(scene, false)) {
            story_scope = "@story:" + scene;
            if (!id.empty() && is_skill_number(id)) story_scope += "/dialog:" + id;
        }
    }
    std::string stable_id = skill_root;
    if (stable_id.empty()) stable_id = table_scope;
    if (stable_id.empty()) stable_id = story_scope;
    if (stable_id.empty() && !id.empty()) stable_id = table_identity(path_, id);

    const bool gift_scope =
        starts_with(table_scope, "@ego-gift:") && table_scope.find('/') == std::string::npos;
    const bool event_scope =
        starts_with(table_scope, "@action-event:") && table_scope.find('/') == std::string::npos;
    const bool guide_scope =
        starts_with(table_scope, "@abnormality-guide:") && table_scope.find('/') == std::string::npos;
    const bool dialogue_scope =
        starts_with(table_scope, "@rpg-dialogue:") && table_scope.find('/') == std::string::npos;

    const auto keys = original->keys();
    for (const std::string &field : keys) {
        const JsonPtr source = original->opt(field);
        const JsonPtr target = translated->opt(field);
        const std::string child_context = object_context + "." + field;
        if (field == "simpleDesc" && gift_scope) {
            if (source && target && source->is_array() && target->is_array()) {
                if (!collect_gift_abilities(source, target, child_context, depth + 1, table_scope,
                                            error)) {
                    return false;
                }
            }
            continue;
        }
        const bool event_options = field == "options" && event_scope;
        const bool guide_stories = field == "storyList" && guide_scope;
        const bool rpg_dialogue = field == "texts" && dialogue_scope;
        if ((event_options || guide_stories || rpg_dialogue)) {
            if (source && target && source->is_array() && target->is_array()) {
                const char *identity_field = guide_stories ? "level" : (rpg_dialogue ? "index" : nullptr);
                if (!collect_table_children(source, target, child_context, depth + 1, table_scope,
                                            identity_field, error)) {
                    return false;
                }
            }
            continue;
        }
        if (source && target && source->is_string() && target->is_string() &&
            is_display_field(field)) {
            IndexRecord record;
            record.path = path_;
            record.context = object_context;
            record.field = field;
            record.source = source->as_string();
            record.translation = target->as_string();
            record.stable_id = stable_id;
            records_->push_back(record);
            if (edits_) {
                edits_->push_back([original, field, target]() { original->put(field, target); });
            }
            continue;
        }
        if (source && (source->is_object() || source->is_array())) {
            Scope nested;
            nested.display_array = is_string_array_field(field) ? &field : nullptr;
            nested.array_owner = stable_id.empty() ? nullptr : &stable_id;
            const std::string lower = lower_ascii(field);
            const bool skill_child = !skill_root.empty() && is_skill_child_field(lower);
            nested.skill_context = skill_child ? &skill_root : nullptr;
            if (skill_child) nested.skill_relation = &lower;
            if (!collect(source, target, child_context, depth + 1, nested, error)) return false;
        }
    }
    return true;
}

bool Collector::collect_array(const JsonPtr &original, const JsonPtr &translated,
                              const std::string &context, int depth, const Scope &scope,
                              std::string *error) {
    if (scope.display_array != nullptr) {
        if (original->length() != translated->length()) return true;
        for (size_t index = 0; index < original->length(); ++index) {
            const JsonPtr source = original->at(index);
            const JsonPtr target = translated->at(index);
            if (!source || !target || !source->is_string() || !target->is_string()) return true;
        }
        for (size_t index = 0; index < original->length(); ++index) {
            const JsonPtr source = original->at(index);
            const JsonPtr target = translated->at(index);
            IndexRecord record;
            record.path = path_;
            record.context = context + "[" + std::to_string(index) + "]";
            record.field = *scope.display_array;
            record.source = source->as_string();
            record.translation = target->as_string();
            if (scope.array_owner) record.stable_id = *scope.array_owner;
            records_->push_back(record);
            if (edits_) {
                edits_->push_back([original, index, target]() { original->set_at(index, target); });
            }
        }
        return true;
    }

    // Group object rows by identity so repeated IDs never pair by position.
    std::unordered_map<std::string, std::vector<JsonPtr>> sources;
    std::unordered_map<std::string, std::vector<JsonPtr>> targets;
    for (const JsonPtr &item : original->items()) {
        if (!item || !item->is_object()) continue;
        const std::string identity = identity_of(item);
        if (identity.empty()) continue;
        sources[identity].push_back(item);
    }
    for (const JsonPtr &item : translated->items()) {
        if (!item || !item->is_object()) continue;
        const std::string identity = identity_of(item);
        if (identity.empty()) continue;
        targets[identity].push_back(item);
    }
    const bool story =
        substring_before_last(path_, '/').size() >= 10 &&
        substring_before_last(path_, '/')
                .compare(substring_before_last(path_, '/').size() - 10, 10, "/StoryData") == 0;
    for (size_t index = 0; index < original->length(); ++index) {
        const JsonPtr source = original->at(index);
        std::string key;
        if (source && source->is_object()) key = identity_of(source);
        JsonPtr target;
        if (original.get() == translated.get()) {
            target = source;
        } else {
            target = snapshot_target(source);
        }
        if (!target) {
            if (original.get() != translated.get() && !key.empty() && source->is_object()) {
                const auto source_group = sources.find(key);
                const auto target_group = targets.find(key);
                const size_t source_count = source_group == sources.end() ? 0 : source_group->second.size();
                const size_t target_count = target_group == targets.end() ? 0 : target_group->second.size();
                if (source_count == 1 && target_count == 1) {
                    target = target_group->second.front();
                } else {
                    const DuplicateIdentity metadata = duplicate_identity(source, story);
                    if (metadata.valid) {
                        size_t matching_sources = 0;
                        for (const JsonPtr &candidate : source_group->second) {
                            if (same_duplicate(duplicate_identity(candidate, story), metadata)) {
                                ++matching_sources;
                            }
                        }
                        if (matching_sources == 1) {
                            JsonPtr single;
                            size_t found = 0;
                            for (const JsonPtr &candidate : target_group->second) {
                                if (same_duplicate(duplicate_identity(candidate, story), metadata)) {
                                    single = candidate;
                                    ++found;
                                }
                            }
                            if (found == 1) target = single;
                        }
                    }
                }
            } else if (original->length() == translated->length()) {
                const JsonPtr candidate = translated->at(index);
                const bool identified = candidate && candidate->is_object() &&
                                        !identity_of(candidate).empty();
                if (!identified) target = candidate;
            }
        }
        std::string nested_skill;
        if (scope.skill_context && scope.skill_relation) {
            const std::string &relation = *scope.skill_relation;
            if (relation == "levellist" && source && source->is_object()) {
                std::string level;
                if (id_text(source->opt("level"), &level) && is_skill_number(level)) {
                    nested_skill = *scope.skill_context + "/level:" + level;
                }
            } else if (relation == "coinlist") {
                nested_skill = *scope.skill_context + "/coin:" + std::to_string(index);
            } else if (relation == "coindescs") {
                nested_skill = *scope.skill_context + "/text:" + std::to_string(index);
            }
        }
        Scope nested;
        if (!nested_skill.empty()) nested.skill_context = &nested_skill;
        if (!collect(source, target, context + "[" + std::to_string(index) + "]", depth + 1, nested,
                     error)) {
            return false;
        }
    }
    return true;
}

bool Collector::collect_gift_abilities(const JsonPtr &original, const JsonPtr &translated,
                                       const std::string &context, int depth,
                                       const std::string &parent, std::string *error) {
    struct Group {
        std::string id;
        std::vector<JsonPtr> rows;
    };
    auto groups = [](const JsonPtr &array, std::unordered_map<std::string, Group> *out) {
        for (const JsonPtr &row : array->items()) {
            if (!row || !row->is_object()) continue;
            std::string id;
            if (!id_text(row->opt("abilityID"), &id) || !is_skill_number(id)) continue;
            Group &group = (*out)[id];
            group.id = id;
            group.rows.push_back(row);
        }
    };
    std::unordered_map<std::string, Group> sources;
    std::unordered_map<std::string, Group> targets;
    groups(original, &sources);
    groups(translated, &targets);

    std::unordered_map<const Json *, JsonPtr> resolved;
    const bool same_document = original.get() == translated.get();
    if (!same_document) {
        for (const auto &entry : sources) {
            const auto target_found = targets.find(entry.first);
            if (target_found == targets.end()) continue;
            const std::vector<JsonPtr> &source_group = entry.second.rows;
            const std::vector<JsonPtr> &target_group = target_found->second.rows;
            for (const JsonPtr &source : source_group) {
                size_t source_matches = 0;
                for (const JsonPtr &candidate : source_group) {
                    if (shared::equivalent(non_display_metadata(source),
                                           non_display_metadata(candidate))) ++source_matches;
                }
                if (source_matches != 1) continue;
                JsonPtr chosen;
                size_t target_matches = 0;
                for (const JsonPtr &candidate : target_group) {
                    if (shared::equivalent(non_display_metadata(source),
                                           non_display_metadata(candidate))) {
                        chosen = candidate;
                        ++target_matches;
                    }
                }
                if (target_matches == 1) resolved[source.get()] = chosen;
            }
            // Translation may spell out an implicit quantity; the last remaining
            // pair is stable through parent + ability identity only.
            if (source_group.size() > 1 && source_group.size() == target_group.size()) {
                JsonPtr remaining_source;
                size_t source_left = 0;
                for (const JsonPtr &candidate : source_group) {
                    if (resolved.find(candidate.get()) == resolved.end()) {
                        remaining_source = candidate;
                        ++source_left;
                    }
                }
                JsonPtr remaining_target;
                size_t target_left = 0;
                for (const JsonPtr &candidate : target_group) {
                    bool used = false;
                    for (const auto &pair : resolved) {
                        if (pair.second.get() == candidate.get()) {
                            used = true;
                            break;
                        }
                    }
                    if (!used) {
                        remaining_target = candidate;
                        ++target_left;
                    }
                }
                if (source_left == 1 && target_left == 1 && display_shape(remaining_source) ==
                                                              display_shape(remaining_target) &&
                    shared::equivalent(non_display_metadata(remaining_source),
                                       non_display_metadata(remaining_target))) {
                    resolved[remaining_source.get()] = remaining_target;
                }
            }
        }
    }

    for (size_t index = 0; index < original->length(); ++index) {
        const JsonPtr source = original->at(index);
        if (!source || !source->is_object()) continue;
        std::string id;
        if (!id_text(source->opt("abilityID"), &id) || !is_skill_number(id)) continue;
        JsonPtr target;
        if (same_document) {
            target = source;
        } else {
            target = snapshot_target(source);
            if (!target) {
                const auto found = resolved.find(source.get());
                if (found == resolved.end()) continue;
                target = found->second;
            }
        }
        if (!shared::equivalent(non_display_metadata(source), non_display_metadata(target))) continue;
        Scope nested;
        const std::string scope = parent + "/ability:" + id;
        nested.object_scope = &scope;
        if (!collect(source, target, context + "[" + std::to_string(index) + "]", depth + 1, nested,
                     error)) {
            return false;
        }
    }
    return true;
}

bool Collector::collect_table_children(const JsonPtr &original, const JsonPtr &translated,
                                       const std::string &context, int depth,
                                       const std::string &parent, const char *identity_field,
                                       std::string *error) {
    if (identity_field == nullptr && original->length() != translated->length()) return true;
    std::unordered_map<std::string, std::vector<JsonPtr>> sources;
    std::unordered_map<std::string, std::vector<JsonPtr>> targets;
    if (identity_field != nullptr) {
        auto group = [identity_field](const JsonPtr &array,
                                      std::unordered_map<std::string, std::vector<JsonPtr>> *out) {
            for (const JsonPtr &row : array->items()) {
                if (!row || !row->is_object()) continue;
                std::string value;
                if (!id_text(row->opt(identity_field), &value) || !is_skill_number(value)) continue;
                (*out)[value].push_back(row);
            }
        };
        group(original, &sources);
        group(translated, &targets);
    }
    for (size_t index = 0; index < original->length(); ++index) {
        const JsonPtr source = original->at(index);
        if (!source || !source->is_object()) continue;
        std::string identity;
        if (identity_field != nullptr) id_text(source->opt(identity_field), &identity);
        JsonPtr target = snapshot_target(source);
        if (!target) {
            if (identity_field != nullptr) {
                const auto source_group = sources.find(identity);
                if (identity.empty() || source_group == sources.end() ||
                    source_group->second.size() != 1) {
                    continue;
                }
                const auto target_group = targets.find(identity);
                if (target_group == targets.end() || target_group->second.size() != 1) continue;
                target = target_group->second.front();
            } else {
                const JsonPtr candidate = translated->at(index);
                if (!candidate || !candidate->is_object()) continue;
                target = candidate;
            }
        }
        if (!shared::equivalent(non_display_metadata(source), non_display_metadata(target))) continue;
        std::string key;
        if (identity_field != nullptr && std::string(identity_field) == "level") {
            key = parent + "/level:" + identity;
        } else if (identity_field != nullptr && std::string(identity_field) == "index") {
            key = parent + "/text:" + identity;
        } else {
            key = parent + "/option:" + std::to_string(index);
        }
        Scope nested;
        nested.object_scope = &key;
        if (!collect(source, target, context + "[" + std::to_string(index) + "]", depth + 1, nested,
                     error)) {
            return false;
        }
    }
    return true;
}

bool collect_released(const JsonPtr &value, int depth, const std::string *display_array,
                      std::vector<std::string> *texts, std::string *error) {
    if (depth > kMaxDepth) {
        *error = "pairer_nesting_limit";
        return false;
    }
    if (!value) return true;
    if (value->is_object()) {
        for (const auto &member : value->members()) {
            const JsonPtr child = member.second;
            if (child && child->is_string()) {
                if (is_display_field(member.first)) {
                    texts->push_back(child->as_string());
                }
                continue;
            }
            const bool is_array_field = is_string_array_field(member.first);
            const std::string *nested = is_array_field ? &member.first : nullptr;
            if (!collect_released(child, depth + 1, nested, texts, error)) return false;
        }
        return true;
    }
    if (value->is_array()) {
        for (const JsonPtr &item : value->items()) {
            if (!collect_released(item, depth + 1, display_array, texts, error)) return false;
        }
        return true;
    }
    if (value->is_string() && display_array != nullptr) {
        texts->push_back(value->as_string());
    }
    return true;
}

bool parse_document(const std::string &text, JsonPtr *out, std::string *error) {
    std::string body = text;
    if (body.compare(0, 3, "\xEF\xBB\xBF") == 0) body = body.substr(3);
    if (body.empty()) {
        *error = "json_empty";
        return false;
    }
    const shared::ParseOutcome parsed = shared::parse(body);
    if (!parsed.ok) {
        *error = parsed.error;
        return false;
    }
    if (!parsed.value || (!parsed.value->is_object() && !parsed.value->is_array())) {
        *error = "resource_root_type";
        return false;
    }
    *out = parsed.value;
    return true;
}

} // namespace

bool decode_snapshot(const JsonPtr &value, DecodedSnapshot *out, std::string *error) {
    static const char kMarker[] = "_bybusTextSnapshot";
    static const char kArray[] = "_bybusArray";
    out->document = value;
    out->source_hash.clear();
    if (!value || !value->is_object() || !value->has(kMarker)) return true;
    const JsonPtr meta = value->opt(kMarker);
    if (!meta || !meta->is_object()) {
        *error = "snapshot_marker_invalid";
        return false;
    }
    const JsonPtr schema = meta->opt("schema");
    const JsonPtr digest = meta->opt("sha256");
    if (!schema || !schema->is_integral() || schema->as_int() != 1 || !digest ||
        !digest->is_string() || !shared::is_sha256_hex(digest->as_string())) {
        *error = "snapshot_marker_invalid";
        return false;
    }
    out->source_hash = digest->as_string();
    value->remove(kMarker);
    const JsonPtr wrapped = value->opt(kArray);
    if (value->length() == 1 && wrapped && wrapped->is_array()) {
        out->document = wrapped;
    }
    return true;
}

bool encode_snapshot(const std::string &original_text, const JsonPtr &document, std::string *out,
                     std::string *error) {
    static const char kMarker[] = "_bybusTextSnapshot";
    static const char kArray[] = "_bybusArray";
    JsonPtr root = document;
    if (!root || !root->is_object()) {
        root = Json::object();
        root->put(kArray, document ? document : Json::null());
    }
    if (root->has(kMarker)) {
        *error = "snapshot_marker_present";
        return false;
    }
    JsonPtr meta = Json::object();
    meta->put_int("schema", 1);
    meta->put_string("sha256", shared::Sha256::hex(original_text));
    root->put(kMarker, meta);
    *out = root->dump();
    root->remove(kMarker);
    return true;
}

namespace {

bool matching_structure_impl(const JsonPtr &original, const JsonPtr &translated, int depth,
                             bool string_array, SnapshotBinding *pairs) {
    if (depth > kMaxDepth) return false;
    if (!original || !translated) return false;
    if (original->is_object() && translated->is_object()) {
        for (const auto &member : translated->members()) {
            if (!original->has(member.first)) return false;
        }
        (*pairs)[original.get()] = translated;
        for (const auto &member : original->members()) {
            const JsonPtr child = member.second;
            if (child && child->is_string() && is_display_field(member.first)) {
                if (translated->has(member.first)) {
                    const JsonPtr other = translated->opt(member.first);
                    if (!other || !other->is_string()) return false;
                }
                continue;
            }
            if (!translated->has(member.first)) return false;
            if (!matching_structure_impl(child, translated->opt(member.first), depth + 1,
                                         is_string_array_field(member.first), pairs)) {
                return false;
            }
        }
        return true;
    }
    if (original->is_array() && translated->is_array()) {
        if (original->length() != translated->length()) return false;
        (*pairs)[original.get()] = translated;
        for (size_t i = 0; i < original->length(); ++i) {
            const JsonPtr left = original->at(i);
            const JsonPtr right = translated->at(i);
            if (string_array && left && left->is_string()) {
                if (!right || !right->is_string()) return false;
                continue;
            }
            if (!matching_structure_impl(left, right, depth + 1, false, pairs)) return false;
        }
        return true;
    }
    if (original->is_number() && translated->is_number()) {
        // Decimal comparison so 1 and 1.0 bind, unlike equivalent().
        return original->as_double() == translated->as_double();
    }
    if (original->is_string() && translated->is_string()) {
        return original->as_string() == translated->as_string();
    }
    return shared::equivalent(original, translated);
}

} // namespace

bool bind_snapshot(const std::string &original_text, const JsonPtr &original,
                   const DecodedSnapshot &decoded, SnapshotBinding *out) {
    out->clear();
    if (decoded.source_hash.empty()) return false;
    if (decoded.source_hash != shared::Sha256::hex(original_text)) return false;
    return matching_structure_impl(original, decoded.document, 0, false, out);
}

std::string JsonPairer::source_table_name(const std::string &path) { return table_name(path); }

bool JsonPairer::pair(const std::string &path, const std::string &original_json,
                      const std::string &translation_json, PairResult *out, std::string *error) {
    JsonPtr original;
    JsonPtr translated_root;
    if (!parse_document(original_json, &original, error)) return false;
    if (!parse_document(translation_json, &translated_root, error)) return false;
    DecodedSnapshot decoded;
    if (!decode_snapshot(translated_root, &decoded, error)) return false;
    SnapshotBinding binding;
    const bool bound = bind_snapshot(original_json, original, decoded, &binding);

    const int source_fields = count_fields(original, 0, nullptr);
    if (source_fields < 0) {
        *error = "pairer_nesting_limit";
        return false;
    }
    if (source_fields == 0 &&
        !(is_explicitly_empty(original) && is_explicitly_empty(decoded.document))) {
        *error = "resource_unrecognized_empty";
        return false;
    }
    PairResult result;
    Scope scope;
    Collector collector(path, &result.records, bound ? &binding : nullptr, nullptr);
    if (!collector.collect(original, decoded.document, "$", 0, scope, error)) return false;
    result.source_fields = source_fields;
    result.matched_fields = static_cast<int>(result.records.size());
    *out = std::move(result);
    return true;
}

bool JsonPairer::merge(const std::string &path, const std::string &original_json,
                       const std::string &translation_json, MergeResult *out, std::string *error) {
    JsonPtr original;
    JsonPtr translated_root;
    if (!parse_document(original_json, &original, error)) return false;
    if (!parse_document(translation_json, &translated_root, error)) return false;
    DecodedSnapshot decoded;
    if (!decode_snapshot(translated_root, &decoded, error)) return false;
    SnapshotBinding binding;
    const bool bound = bind_snapshot(original_json, original, decoded, &binding);

    PairResult result;
    std::vector<std::function<void()>> edits;
    Scope scope;
    Collector collector(path, &result.records, bound ? &binding : nullptr, &edits);
    if (!collector.collect(original, decoded.document, "$", 0, scope, error)) return false;
    result.source_fields = count_fields(original, 0, nullptr);
    if (result.source_fields < 0) {
        *error = "pairer_nesting_limit";
        return false;
    }
    result.matched_fields = static_cast<int>(result.records.size());
    bool differs = false;
    for (const IndexRecord &record : result.records) {
        if (record.source != record.translation) {
            differs = true;
            break;
        }
    }
    for (auto &edit : edits) edit();
    out->changed = differs;
    out->source_fields = result.source_fields;
    out->matched_fields = result.matched_fields;
    out->records = std::move(result.records);
    if (differs) out->content = original->dump();
    return true;
}

bool JsonPairer::released_texts(const std::string &json, std::vector<std::string> *out,
                                std::string *error) {
    JsonPtr root;
    if (!parse_document(json, &root, error)) return false;
    DecodedSnapshot decoded;
    if (!decode_snapshot(root, &decoded, error)) return false;
    out->clear();
    return collect_released(decoded.document, 0, nullptr, out, error);
}

} // namespace bybus::translation
