#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include "heating_program.h"

uint32_t get_ms(void);
char *format_json_alloc(const char *format, ...);
extern const char * const JOURS_FR[];
const char *heating_mode_to_string(heating_mode_t mode);

#endif