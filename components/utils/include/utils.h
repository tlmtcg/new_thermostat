#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

uint32_t get_ms(void);
char *format_json_alloc(const char *format, ...);

#endif