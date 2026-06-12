#include <stdint.h>
#include "esp_timer.h"
#include <stdio.h>
#include "stdarg.h"

uint32_t get_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Formate une chaîne de caractères dynamiquement en une seule passe dans le code principal
char *format_json_alloc(const char *format, ...)
{
    va_list args;
    
    // 1. Premier calcul de la taille requise
    va_start(args, format);
    int len = vsnprintf(NULL, 0, format, args);
    va_end(args);

    if (len < 0)
        return NULL;

    // 2. Allocation sur mesure
    char *buf = malloc(len + 1);
    if (!buf)
        return NULL;

    // 3. Écriture réelle dans le tampon
    va_start(args, format);
    vsnprintf(buf, len + 1, format, args);
    va_end(args);

    return buf;
}
