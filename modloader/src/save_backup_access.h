#pragma once
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace save_backup_access {
constexpr gid_t kExternalDataGroup = 1078; // Android AID_EXT_DATA_RW
struct Result {
    unsigned checked = 0, changed = 0, errors = 0;
    int error = 0;
    std::string path;
};
inline mode_t readable_mode(mode_t mode, bool directory, gid_t group) {
    // Preserve every existing write bit. Never grant new write permissions.
    const mode_t read = directory ? (S_IRGRP | S_IXGRP) : S_IRGRP;
    if (group == kExternalDataGroup)
        return (mode & 07777) | read | (directory ? S_ISGID : 0);
    // App-created files can have the app's private GID. Without CAP_CHOWN the
    // owner can grant read/search within Android's package-scoped external tree.
    return (mode & 07777) | (directory ? (S_IROTH | S_IXOTH) : S_IROTH);
}
inline void failure(Result& result, const std::string& path, int error) {
    ++result.errors;
    if (!result.error) { result.error = error; result.path = path; }
}
inline void walk(int fd, const std::string& path, unsigned depth, Result& result) {
    if (depth > 24 || ++result.checked > 20000) { failure(result, path, EFBIG); return; }
    struct stat info{};
    if (fstat(fd, &info)) { failure(result, path, errno); return; }
    const bool directory = S_ISDIR(info.st_mode);
    if (!directory && !S_ISREG(info.st_mode)) { failure(result, path, EINVAL); return; }
    if (info.st_uid == geteuid()) {
        const auto wanted = readable_mode(info.st_mode, directory, info.st_gid);
        if ((info.st_mode & 07777) != wanted) {
            if (fchmod(fd, wanted)) failure(result, path, errno);
            else {
                struct stat after{};
                if (fstat(fd, &after)) failure(result, path, errno);
                else if ((after.st_mode & 07777) != wanted) failure(result, path, EACCES);
                else ++result.changed;
            }
        }
    }
    if (!directory || result.checked > 20000) return;
    const int copy = dup(fd);
    DIR* stream = copy < 0 ? nullptr : fdopendir(copy);
    if (!stream) { if (copy >= 0) close(copy); failure(result, path, errno); return; }
    while (auto* entry = readdir(stream)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        const std::string child = path.empty() ? entry->d_name : path + "/" + entry->d_name;
        struct stat before{};
        if (fstatat(fd, entry->d_name, &before, AT_SYMLINK_NOFOLLOW)) {
            failure(result, child, errno); continue;
        }
        if (!S_ISDIR(before.st_mode) && !S_ISREG(before.st_mode)) {
            failure(result, child, ELOOP); continue;
        }
        const int nested = openat(fd, entry->d_name,
            O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC | (S_ISDIR(before.st_mode) ? O_DIRECTORY : 0));
        if (nested < 0) { failure(result, child, errno); continue; }
        struct stat opened{};
        if (fstat(nested, &opened) || opened.st_ino != before.st_ino || opened.st_dev != before.st_dev)
            failure(result, child, EAGAIN);
        else walk(nested, child, depth + 1, result);
        close(nested);
        if (result.checked > 20000) break;
    }
    closedir(stream);
}
inline Result repair(const char* root) {
    Result result;
    const int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) failure(result, "", errno);
        return result;
    }
    walk(fd, "", 0, result);
    close(fd);
    return result;
}
}
