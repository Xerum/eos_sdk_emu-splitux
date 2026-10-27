#include "internal/logging.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_log_mutex;
static BOOL g_log_mutex_initialized = FALSE;
#else
#include <pthread.h>
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static LogLevel g_log_level = LOG_LEVEL_INFO;
static FILE* g_log_file = NULL;
static int g_log_lazy_tried = 0;  // log_write opens the log itself if nothing called log_init yet

static const char* level_names[] = {
    "NONE", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"
};

LogLevel log_level_from_env(void) {
    const char* lvl = getenv("EOSLAN_LOG_LEVEL");
    if (!lvl || !lvl[0]) return LOG_LEVEL_INFO;
    if (lvl[0] >= '0' && lvl[0] <= '5') return (LogLevel)(lvl[0] - '0');
    if (_stricmp(lvl, "none") == 0)  return LOG_LEVEL_NONE;
    if (_stricmp(lvl, "error") == 0) return LOG_LEVEL_ERROR;
    if (_stricmp(lvl, "warn") == 0)  return LOG_LEVEL_WARN;
    if (_stricmp(lvl, "debug") == 0) return LOG_LEVEL_DEBUG;
    if (_stricmp(lvl, "trace") == 0) return LOG_LEVEL_TRACE;
    return LOG_LEVEL_INFO;
}

void log_init(LogLevel level, const char* file) {
    g_log_level = level;
    g_log_lazy_tried = 1;
    if (g_log_file) {
        return;  // already opened (lazily, before EOS_Initialize): keep it, just apply the level
    }

#ifdef _WIN32
    if (!g_log_mutex_initialized) {
        InitializeCriticalSection(&g_log_mutex);
        g_log_mutex_initialized = TRUE;
    }
#endif

    if (file && strlen(file) > 0) {
        g_log_file = fopen(file, "a");
        if (!g_log_file) {
            fprintf(stderr, "[EOSLAN] Failed to open log file: %s (errno: %d)\n", file, errno);
        } else {
            fprintf(stderr, "[EOSLAN] Log file opened: %s\n", file);
        }
    }
}

void log_shutdown(void) {
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }

#ifdef _WIN32
    if (g_log_mutex_initialized) {
        DeleteCriticalSection(&g_log_mutex);
        g_log_mutex_initialized = FALSE;
    }
#endif
}

void log_set_level(LogLevel level) {
    g_log_level = level;
}

void log_write(LogLevel level, const char* func, const char* fmt, ...) {
    // Games may call into the SDK (EOS_GetVersion, ...) before EOS_Initialize, and
    // when Initialize is never reached those calls were the only clue -- unlogged.
    if (!g_log_file && !g_log_lazy_tried) {
        const char* path = getenv("EOSLAN_LOG_PATH");
        log_init(log_level_from_env(), (path && path[0]) ? path : "eos-lan.log");
    }
    if (level > g_log_level) return;

#ifdef _WIN32
    if (g_log_mutex_initialized) {
        EnterCriticalSection(&g_log_mutex);
    }
#else
    pthread_mutex_lock(&g_log_mutex);
#endif

    // Get timestamp
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm);

    // Format message
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    // Output format: [TIME] [LEVEL] [FUNC] message
    char output[2200];
    snprintf(output, sizeof(output), "[%s] [%-5s] [%s] %s\n",
             timestamp, level_names[level], func, message);

    // Write to stderr
    fputs(output, stderr);

    // Write to file if configured
    if (g_log_file) {
        fputs(output, g_log_file);
        fflush(g_log_file);
    }

#ifdef _WIN32
    if (g_log_mutex_initialized) {
        LeaveCriticalSection(&g_log_mutex);
    }
#else
    pthread_mutex_unlock(&g_log_mutex);
#endif
}
