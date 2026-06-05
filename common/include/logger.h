#pragma once
// Lightweight logger API — ARM9 writes to fat:/Alone/log.txt, ARM7 no-op.

#ifdef ARM9
#include <stdarg.h>
#include <stdio.h>

// Initialize logger (call after FAT is mounted)
void logger_init(void);

// Print formatted message to log file (and mirror to console)
void logger_printf(const char* fmt, ...);

// Periodic heartbeat; call once per frame from the main loop.
void logger_periodic(unsigned int frame);

#else
// On ARM7 or non-ARM9 builds provide no-op stubs so includes are safe.
static inline void logger_init(void) { (void)0; }
static inline void logger_printf(const char* fmt, ...) { (void)fmt; }
static inline void logger_periodic(unsigned int frame) { (void)frame; }
#endif
