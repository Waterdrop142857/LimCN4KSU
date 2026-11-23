// service/generation_store.cpp
// Responsibility: implementation of the generation directory set and the
// atomic `active` pointer. Filesystem moves happen outside any shared lock;
// the class holds no shared state of its own.
#include "service/generation_store.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "shared/fs.h"
#include "shared/sha256.h"

namespace bybus::service {
namespace {

constexpr const char *kTempPrefix = ".tmp-";

bool is_generation_name(const std::string &name) {
    return shared::is_sha256_hex(name);
}

int64_t directory_mtime_millis(const std::string &path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return 0;
    return static_cast<int64_t>(info.st_mtime) * 1000;
}

} // namespace

GenerationStore::GenerationStore(ServicePaths paths) : paths_(std::move(paths)) {}

shared::Code GenerationStore::create_temp(std::string *temp_dir) const {
    if (!temp_dir) return shared::Code::InvalidArgument;
    const std::string created = paths_.generations() + "/" + kTempPrefix + random_hex(12);
    const shared::Code made = shared::make_dirs(created, 0700);
    if (made != shared::Code::Ok) return made;
    *temp_dir = created;
    return shared::Code::Ok;
}

shared::Code GenerationStore::publish(const std::string &temp_dir,
                                      const std::string &generation, bool activate) const {
    if (!is_generation_name(generation)) return shared::Code::InvalidArgument;
    if (!inside_state_root(paths_, temp_dir)) return shared::Code::InvalidArgument;
    const std::string target = paths_.generation_dir(generation);
    if (shared::exists(target)) {
        (void)shared::remove_tree(temp_dir);
    } else {
        if (shared::sync_dir(temp_dir) != shared::Code::Ok) return shared::Code::IoError;
        const shared::Code synced = shared::sync_dir(paths_.generations());
        if (synced != shared::Code::Ok) return synced;
        if (::rename(temp_dir.c_str(), target.c_str()) != 0) return shared::Code::IoError;
        const shared::Code synced_after = shared::sync_dir(paths_.generations());
        if (synced_after != shared::Code::Ok) return synced_after;
    }
    return activate ? write_active(generation) : shared::Code::Ok;
}

shared::Code GenerationStore::write_active(const std::string &generation) const {
    if (!is_generation_name(generation)) return shared::Code::InvalidArgument;
    const shared::Code wrote = write_atomic(paths_.active_temp(), generation + "\n");
    if (wrote != shared::Code::Ok) return wrote;
    return shared::atomic_replace(paths_.active_temp(), paths_.active());
}

shared::Code GenerationStore::read_active(std::string *generation) const {
    if (!generation) return shared::Code::InvalidArgument;
    generation->clear();
    std::string text;
    const shared::Code read = shared::read_file(paths_.active(), 256, &text);
    if (read == shared::Code::GameMissing) return shared::Code::Ok;
    if (read != shared::Code::Ok) return read;
    std::string trimmed;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        trimmed.push_back(c);
    }
    if (trimmed.empty()) return shared::Code::Ok;
    if (!is_generation_name(trimmed)) return shared::Code::SnapshotInvalid;
    *generation = trimmed;
    return shared::Code::Ok;
}

shared::Code GenerationStore::clear_active() const {
    if (::unlink(paths_.active().c_str()) != 0 && errno != ENOENT) return shared::Code::IoError;
    const shared::Code synced = shared::sync_dir(paths_.state_root);
    if (synced != shared::Code::Ok) return synced;
    return shared::Code::Ok;
}

ActiveSnapshot GenerationStore::inspect_active() const {
    ActiveSnapshot snapshot;
    std::string generation;
    const shared::Code read = read_active(&generation);
    if (read != shared::Code::Ok) {
        snapshot.code = read;
        return snapshot;
    }
    if (generation.empty()) {
        snapshot.present = false;
        snapshot.code = shared::Code::Ok;
        return snapshot;
    }
    snapshot.present = true;
    snapshot.generation = generation;
    const std::string dir = dir_of(generation);
    if (!shared::is_directory(dir)) {
        snapshot.code = shared::Code::SnapshotInvalid;
        return snapshot;
    }
    std::string text;
    const shared::Code manifest_read =
        shared::read_file(dir + "/" + translation::kManifestName, kMaxManifestBytes, &text);
    if (manifest_read != shared::Code::Ok) {
        snapshot.code = manifest_read == shared::Code::ArchiveLimit ? shared::Code::SnapshotInvalid
                                                                   : manifest_read;
        return snapshot;
    }
    translation::SnapshotManifest manifest;
    std::string error;
    if (!translation::parse_manifest(text, &manifest, &error)) {
        snapshot.code = shared::Code::SnapshotInvalid;
        return snapshot;
    }
    if (manifest.generation != generation) {
        snapshot.code = shared::Code::SnapshotInvalid;
        return snapshot;
    }
    translation::SnapshotIndex index;
    if (!index.load(manifest, &error)) {
        snapshot.code = shared::Code::SnapshotInvalid;
        return snapshot;
    }
    snapshot.table_count = static_cast<uint32_t>(manifest.entries.size());
    snapshot.source_id = manifest.source_id;
    snapshot.archive_sha256 = manifest.archive_sha256;
    snapshot.font_sha256 = manifest.font_sha256;
    snapshot.built_at_millis = built_at_millis(generation);
    snapshot.valid = true;
    snapshot.code = shared::Code::Ok;
    return snapshot;
}

shared::Code GenerationStore::cleanup_temps() const {
    std::vector<std::string> names;
    const shared::Code listed = shared::list_dir(paths_.generations(), kMaxGenerationsListed,
                                                &names);
    if (listed == shared::Code::GameMissing) return shared::Code::Ok;
    if (listed != shared::Code::Ok) return listed;
    for (const std::string &name : names) {
        if (name.compare(0, std::string(kTempPrefix).size(), kTempPrefix) != 0) continue;
        (void)shared::remove_tree(paths_.generations() + "/" + name);
    }
    return shared::Code::Ok;
}

shared::Code GenerationStore::prune(uint32_t *removed,
                                    const std::string &protected_generation) const {
    if (removed) *removed = 0;
    std::string active;
    const shared::Code read = read_active(&active);
    if (read != shared::Code::Ok) return read;
    std::vector<std::pair<int64_t, std::string>> entries;
    for (const std::string &name : list()) {
        entries.emplace_back(built_at_millis(name), name);
    }
    std::sort(entries.begin(), entries.end(),
              [](const std::pair<int64_t, std::string> &left,
                 const std::pair<int64_t, std::string> &right) {
                  if (left.first != right.first) return left.first > right.first;
                  return left.second > right.second;
              });
    uint32_t kept_others = 0;
    for (const auto &entry : entries) {
        if (entry.second == active) continue;
        if (entry.second == protected_generation) {
            ++kept_others;
            continue;
        }
        if (kept_others < kKeepGenerations) {
            ++kept_others;
            continue;
        }
        const std::string dir = paths_.generation_dir(entry.second);
        if (!inside_state_root(paths_, dir)) continue;
        if (shared::remove_tree(dir) == shared::Code::Ok && removed) ++(*removed);
    }
    return shared::Code::Ok;
}

std::vector<std::string> GenerationStore::list() const {
    std::vector<std::string> names;
    const shared::Code listed = shared::list_dir(paths_.generations(), kMaxGenerationsListed,
                                                &names);
    if (listed != shared::Code::Ok) return std::vector<std::string>();
    std::vector<std::pair<int64_t, std::string>> entries;
    for (const std::string &name : names) {
        if (!is_generation_name(name)) continue;
        entries.emplace_back(built_at_millis(name), name);
    }
    std::sort(entries.begin(), entries.end(),
              [](const std::pair<int64_t, std::string> &left,
                 const std::pair<int64_t, std::string> &right) {
                  if (left.first != right.first) return left.first > right.first;
                  return left.second > right.second;
              });
    std::vector<std::string> generations;
    generations.reserve(entries.size());
    for (const auto &entry : entries) generations.push_back(entry.second);
    return generations;
}

bool GenerationStore::exists(const std::string &generation) const {
    if (!is_generation_name(generation)) return false;
    return shared::is_directory(dir_of(generation));
}

std::string GenerationStore::dir_of(const std::string &generation) const {
    return paths_.generation_dir(generation);
}

int64_t GenerationStore::built_at_millis(const std::string &generation) const {
    if (!is_generation_name(generation)) return 0;
    return directory_mtime_millis(dir_of(generation));
}

} // namespace bybus::service
