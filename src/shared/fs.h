// Small POSIX file helpers shared by the daemon and the snapshot builder.
// Every operation is bounded and reports a protocol code instead of throwing.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shared/status.h"

namespace bybus::shared {

// Reads at most max_bytes; a larger file fails with archive_limit semantics.
Code read_file(const std::string &path, uint64_t max_bytes, std::string *out);

// Writes with O_CREAT|O_TRUNC and fsyncs the file before returning.
Code write_file(const std::string &path, const std::string &content);

// Creates the directory and its parents (mode applied to each new level).
Code make_dirs(const std::string &path, unsigned mode);

// fsyncs a directory so a rename inside it survives a power loss.
Code sync_dir(const std::string &path);

// rename(2) with a directory fsync of the parent.
Code atomic_replace(const std::string &source, const std::string &target);

// Recursively removes a tree. Refuses paths outside the bybus state root.
Code remove_tree(const std::string &path);

bool exists(const std::string &path);
bool is_directory(const std::string &path);
uint64_t file_size(const std::string &path);
// Lists names (not paths) of a directory, sorted, bounded by max_entries.
Code list_dir(const std::string &path, uint32_t max_entries, std::vector<std::string> *names);

// True when `candidate` is a strict descendant of `root` after lexical
// normalisation (no symlink resolution required here).
bool path_is_inside(const std::string &root, const std::string &candidate);

} // namespace bybus::shared
