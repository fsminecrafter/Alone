// Simple file logger for ARM9 — writes to fat:/Alone/log.txt and mirrors to console.
#ifdef ARM9

#include "../common/include/logger.h"
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static FILE* s_logFile = nullptr;

void logger_init(void)
{
    if (s_logFile) return;
    // Ensure FAT is initialized by caller (main does fatInitDefault()).
    s_logFile = fopen("fat:/Alone/log.txt", "a");
    if (!s_logFile) {
        iprintf("logger: failed to open fat:/Alone/log.txt for append\n");
    } else {
        fprintf(s_logFile, "\n---- Log start ----\n");
        fflush(s_logFile);
    }
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

    if (s_logFile) {
        fprintf(s_logFile, "%s", buf);
        fprintf(s_logFile, "\n");
        fflush(s_logFile);
    }
}

#endif // ARM9
