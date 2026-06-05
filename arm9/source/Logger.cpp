// Simple file logger for ARM9 — writes to fat:/Alone/log.txt and mirrors to console.
#ifdef ARM9

#include "../common/include/logger.h"
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static const char* LOG_PATH = "fat:/Alone/log.txt";
static const unsigned int HEARTBEAT_INTERVAL_FRAMES = 300; // About 5 seconds at 60 FPS.

static FILE* s_logFile = nullptr;

static bool logger_open(void)
{
    if (s_logFile) return true;

    s_logFile = fopen(LOG_PATH, "a");
    if (!s_logFile) {
        iprintf("logger: failed to open %s for append\n", LOG_PATH);
        return false;
    }

    return true;
}

static void logger_commit_to_fat(void)
{
    if (!s_logFile) return;

    fflush(s_logFile);
    fclose(s_logFile);
    s_logFile = nullptr;

    // Reopen immediately so normal logger_printf calls can keep appending.
    logger_open();
}

void logger_init(void)
{
    // Ensure FAT is initialized by caller (main does fatInitDefault()).
    if (!logger_open()) return;

    fprintf(s_logFile, "\n---- Log start ----\n");
    logger_commit_to_fat();
}

void logger_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    // Mirror to console
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    iprintf("%s", buf);

    if (logger_open()) {
        fprintf(s_logFile, "%s", buf);
        size_t len = strlen(buf);
        if (len == 0 || buf[len - 1] != '\n') {
            fprintf(s_logFile, "\n");
        }
        fflush(s_logFile);
    }
}

void logger_periodic(unsigned int frame)
{
    if ((frame % HEARTBEAT_INTERVAL_FRAMES) != 0) return;
    if (!logger_open()) return;

    fprintf(s_logFile, "[HEARTBEAT] frame=%u seconds=%u\n",
            frame, frame / 60);
    logger_commit_to_fat();
}

#endif // ARM9
