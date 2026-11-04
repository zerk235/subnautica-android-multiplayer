#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <android/log.h>

#define LOG_TAG "SNMod"
#define MOD_DIR "/sdcard/Android/data/com.unknownworlds.subnautica/files/mod_log"
#define FALLBACK_DIR "/data/data/com.unknownworlds.subnautica/files/mod_log"
#define LOG_NAME "logs/modloader.log"
#define LOG_MAX 4096

static int g_fd = -1;
static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static int ensure_dir(const char* path) {
    char buf[512];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        mkdir(buf, 0770);
        buf[i] = '/';
    }
    return mkdir(buf, 0770);
}

static int try_open(const char* dir, char* used, size_t used_size) {
    char dirbuf[512];
    char path[640];
    snprintf(dirbuf, sizeof(dirbuf), "%s/logs", dir);
    if (ensure_dir(dirbuf) != 0 && errno != EEXIST) return -1;

    snprintf(path, sizeof(path), "%s/%s", dir, LOG_NAME);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0660);
    if (fd >= 0) snprintf(used, used_size, "%s", path);
    return fd;
}

bool mod_log_open() {
    pthread_mutex_lock(&g_lock);
    if (g_fd >= 0) {
        pthread_mutex_unlock(&g_lock);
        return true;
    }

    char used[640] = "";
    int fd = try_open(MOD_DIR, used, sizeof(used));
    if (fd < 0) fd = try_open(FALLBACK_DIR, used, sizeof(used));

    if (fd >= 0) {
        char stamp[64];
        time_t now = time(nullptr);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf);
        char line[128];
        int n = snprintf(line, sizeof(line), "=== log opened %s ===\n", stamp);
        ssize_t ignored = write(fd, line, (size_t)n);
        (void)ignored;
        g_fd = fd;
    }

    bool ok = g_fd >= 0;
    pthread_mutex_unlock(&g_lock);

    if (ok) LOG_INFO("logging to %s", used);
    return ok;
}

void mod_log_write(const char* level, const char* fmt, va_list args) {
    char msg[LOG_MAX];
    vsnprintf(msg, sizeof(msg), fmt, args);

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm_buf;
    localtime_r(&tv.tv_sec, &tm_buf);

    char line[LOG_MAX + 64];
    int n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] %s\n",
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                     (int)(tv.tv_usec / 1000), level, msg);
    if (n <= 0) return;

    __android_log_write(ANDROID_LOG_INFO, LOG_TAG, msg);

    pthread_mutex_lock(&g_lock);
    if (g_fd < 0) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    ssize_t ignored = write(g_fd, line, (size_t)n);
    (void)ignored;
    pthread_mutex_unlock(&g_lock);
}

void mod_logf(const char* level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mod_log_write(level, fmt, args);
    va_end(args);
}