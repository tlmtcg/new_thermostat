#pragma once
#include <stdint.h>
#include "ssd1306_driver.h"

extern const uint8_t font5x7[128][5];

const ssd1306_font_t SSD1306_FONT_5X7 = {
    .bitmap = (const uint8_t *)font5x7,
    .width = 5,
    .height = 7,
    .spacing = 1
};
