#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#define SSD1306_WIDTH        128
#define SSD1306_HEIGHT       64
#define SSD1306_BUFFER_SIZE  1024

typedef struct {
    const uint8_t *bitmap;
    uint8_t width;
    uint8_t height;
    uint8_t spacing;
} ssd1306_font_t;

typedef struct {
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t i2c_dev;
    uint8_t i2c_address;
    uint8_t *buffer;
    bool initialized;
    const ssd1306_font_t *font;
} ssd1306_t;

extern const ssd1306_font_t SSD1306_FONT_5X7;

esp_err_t ssd1306_init(ssd1306_t *lcd, i2c_master_bus_handle_t bus, uint8_t address);
void ssd1306_clear(ssd1306_t *lcd);
esp_err_t ssd1306_update(ssd1306_t *lcd);
void ssd1306_draw_pixel(ssd1306_t *lcd, uint8_t x, uint8_t y, bool color);
void ssd1306_draw_line(ssd1306_t *lcd, uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2, bool color);
void ssd1306_draw_string(ssd1306_t *lcd, uint8_t x, uint8_t y, const char *str);
void ssd1306_clear(ssd1306_t *lcd);