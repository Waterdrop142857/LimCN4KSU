#include "shared/fs.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace bybus::shared {
namespace {

Code errno_code(int value) {
    switch (value) {
        case ENOENT:
        case ENOTDIR:
            return Code::GameMissing;
        case EACCES:
        case EPERM:
            return Code::IoError;
        case ENOSPC:
        case EDQUOT:
            return Code::IoError;
        default:
            return Code::IoError;
    }
}

std::vector<std::string> split_path(const std::string &path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t separator = path.find('/', start);
        const size_t end = separator == std::string::npos ? path.size() : separator;
        if (end > start) parts.push_back(path.substr(start, end - start));
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    return parts;
}

std::string normalise(const std::string &path) {
    const bool absolute = !path.empty() && path.front() == '/';
    std::vector<std::string> stack;
    for (const std::string &part : split_path(path)) {
        if (part == ".") continue;
        if (part == "..") {
            if (!stack.empty()) stack.pop_back();
            continue;
        }
        stack.push_back(part);
    }
    std::string result = absolute ? "/" : "";
    for (size_t i = 0; i < stack.size(); ++i) {
        if (i != 0 || !absolute) result += "/";
        result += stack[i];
    }
    return result.empty() ? (absolute ? "/" : ".") : result;
}

} // namespace

Code read_file(const std::string &path, uint64_t max_bytes, std::string *out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno_code(errno);
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        ::close(fd);
        return errno_code(errno);
    }
    if (!S_ISREG(info.st_mode)) {
        ::close(fd);
        return Code::IoError;
    }
    if (static_cast<uint64_t>(info.st_size) > max_bytes) {
        ::close(fd);
        return Code::ArchiveLimit;
    }
    std::string content;
    content.resize(static_cast<size_t>(info.st_size));
    size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t read_size = ::read(fd, &content[offset], content.size() - offset);
        if (read_size < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return errno_code(errno);
        }
        if (read_size == 0) break;
        offset += static_cast<size_t>(read_size);
    }
    ::close(fd);
    content.resize(offset);
    *out = std::move(content);
    return Code::Ok;
}

Code write_file(const std::string &path, const std::string &content) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return errno_code(errno);
    size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t written = ::write(fd, content.data() + offset, content.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            const int saved = errno;
            ::close(fd);
            return errno_code(saved);
        }
        offset += static_cast<size_t>(written);
    }
    if (::fsync(fd) != 0) {
        const int saved = errno;
        ::close(fd);
        return errno_code(saved);
    }
    ::close(fd);
    return Code::Ok;
}

Code make_dirs(const std::string &path, unsigned mode) {
    if (path.empty()) return Code::InvalidArgument;
    std::string current;
    if (path.front() == '/') current = "/";
    for (const std::string &part : split_path(path)) {
        if (!current.empty() && current.back() != '/') current += "/";
        current += part;
        if (::mkdir(current.c_str(), static_cast<mode_t>(mode)) != 0 && errno != EEXIST) {
            return errno_code(errno);
        }
    }
    return Code::Ok;
}

Code sync_dir(const std::string &path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return errno_code(errno);
    const int result = ::fsync(fd);
    ::close(fd);
    return result == 0 ? Code::Ok : errno_code(errno);
}

Code atomic_replace(const std::string &source, const std::string &target) {
    if (::rename(source.c_str(), target.c_str()) != 0) return errno_code(errno);
    const size_t at = target.rfind('/');
    const std::string parent = at == std::string::npos ? std::string(".") : target.substr(0, at);
    return sync_dir(parent);
}

bool exists(const std::string &path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

bool is_directory(const std::string &path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

uint64_t file_size(const std::string &path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return 0;
    return static_cast<uint64_t>(info.st_size);
}

Code list_dir(const std::string &path, uint32_t max_entries, std::vector<std::string> *names) {
    DIR *dir = ::opendir(path.c_str());
    if (!dir) return errno_code(errno);
    names->clear();
    uint32_t count = 0;
    while (dirent *entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (++count > max_entries) {
            ::closedir(dir);
            return Code::ArchiveLimit;
        }
        names->push_back(name);
    }
    ::closedir(dir);
    std::sort(names->begin(), names->end());
    return Code::Ok;
}

Code remove_tree(const std::string &path) {
    if (!path_is_inside("/data/adb/bybus", path)) return Code::InvalidArgument;
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) return errno == ENOENT ? Code::Ok : errno_code(errno);
    if (!S_ISDIR(info.st_mode)) return ::unlink(path.c_str()) == 0 ? Code::Ok : errno_code(errno);
    std::vector<std::string> names;
    const Code listed = list_dir(path, 100000, &names);
    if (listed != Code::Ok) return listed;
    for (const std::string &name : names) {
        const Code removed = remove_tree(path + "/" + name);
        if (removed != Code::Ok) return removed;
    }
    return ::rmdir(path.c_str()) == 0 ? Code::Ok : errno_code(errno);
}

bool path_is_inside(const std::string &root, const std::string &candidate) {
    const std::string normalised_root = normalise(root);
    const std::string normalised_candidate = normalise(candidate);
    if (normalised_candidate.size() <= normalised_root.size()) return false;
    if (normalised_candidate.compare(0, normalised_root.size(), normalised_root) != 0) return false;
    return normalised_candidate[normalised_root.size()] == '/';
}

} // namespace bybus::shared
