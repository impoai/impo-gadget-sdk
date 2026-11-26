#pragma once
#include <stdarg.h>
#define SHELL_EXPORT_CMD(...)
static inline void *shellGetCurrent(void) { return 0; }
static inline void shellPrint(void *s,const char *f,...) { (void)s;(void)f; }
