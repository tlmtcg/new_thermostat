/**
 * @file ssd1306_driver.c
 * @brief Driver SSD1306 robuste ESP-IDF (ESP32-S3 safe + mutex I2C)
 */

#include "ssd1306_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include "font5x7.h"
#include "i2c_shared.h"

static const char *TAG = "SSD1306_DRV";

#define SSD1306_CONTROL_CMD 0x00
#define SSD1306_CONTROL_DATA 0x40

// buffer vidéo 128x64
static uint8_t video_buffer[SSD1306_BUFFER_SIZE];

// =======================================================
// FORWARD DECL (IMPORTANT => FIX implicit declaration)
// =======================================================
static void ssd1306_draw_char(ssd1306_t *lcd, uint8_t x, uint8_t y, char c);

// =======================================================
// LOW LEVEL I2C CMD (NO MUTEX HERE)
// =======================================================
esp_err_t ssd1306_write_cmd(ssd1306_t *lcd, uint8_t cmd)
{
    if (!lcd || !lcd->i2c_dev)
        return ESP_ERR_INVALID_STATE;

    uint8_t tx[2] = {SSD1306_CONTROL_CMD, cmd};

    return i2c_master_transmit(lcd->i2c_dev, tx, 2, 100);
}

// =======================================================
// INIT DEVICE
// =======================================================
esp_err_t ssd1306_reinit(ssd1306_t *lcd,
                         i2c_master_bus_handle_t bus,
                         uint8_t address)
{
    if (!lcd || !bus)
        return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "SSD1306 init @0x%02X", address);

    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    if (lcd->initialized && lcd->i2c_dev)
    {
        i2c_master_bus_rm_device(lcd->i2c_dev);
        lcd->i2c_dev = NULL;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000,
    };

    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &lcd->i2c_dev);
    if (err != ESP_OK)
    {
        xSemaphoreGive(g_i2c_mutex);
        return err;
    }

    lcd->i2c_address = address;

    vTaskDelay(pdMS_TO_TICKS(50));

    // init minimal (tu peux remettre ta séquence complète ensuite)
    if ((err = ssd1306_write_cmd(lcd, 0xAE)) != ESP_OK)
        goto fail;
    if ((err = ssd1306_write_cmd(lcd, 0xA6)) != ESP_OK)
        goto fail;
    if ((err = ssd1306_write_cmd(lcd, 0xAF)) != ESP_OK)
        goto fail;

    lcd->initialized = true;

    xSemaphoreGive(g_i2c_mutex);
    return ESP_OK;

fail:
    xSemaphoreGive(g_i2c_mutex);
    return err;
}

// =======================================================
// INIT PUBLIC
// =======================================================
esp_err_t ssd1306_init(ssd1306_t *lcd,
                       i2c_master_bus_handle_t bus,
                       uint8_t address)
{
    if (!lcd)
        return ESP_ERR_INVALID_ARG;

    memset(lcd, 0, sizeof(*lcd));

    lcd->buffer = video_buffer;
    lcd->bus_handle = bus;
    lcd->font = &SSD1306_FONT_5X7;

    memset(lcd->buffer, 0, SSD1306_BUFFER_SIZE);

    return ssd1306_reinit(lcd, bus, address);
}

// =======================================================
// CLEAR
// =======================================================
void ssd1306_clear(ssd1306_t *lcd)
{
    if (!lcd || !lcd->initialized)
        return;

    memset(lcd->buffer, 0, SSD1306_BUFFER_SIZE);
}

// =======================================================
// UPDATE DISPLAY
// =======================================================
esp_err_t ssd1306_update(ssd1306_t *lcd)
{
    if (!lcd || !lcd->i2c_dev || !lcd->initialized)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    uint8_t page_buf[129];
    page_buf[0] = SSD1306_CONTROL_DATA;

    for (int page = 0; page < 8; page++)
    {
        ssd1306_write_cmd(lcd, 0xB0 + page);
        ssd1306_write_cmd(lcd, 0x00);
        ssd1306_write_cmd(lcd, 0x10);

        memcpy(&page_buf[1],
               &lcd->buffer[page * SSD1306_WIDTH],
               SSD1306_WIDTH);

        err = i2c_master_transmit(lcd->i2c_dev, page_buf, 129, 100);

        if (err != ESP_OK)
            break;
    }

    xSemaphoreGive(g_i2c_mutex);
    return err;
}

// =======================================================
// PIXEL DRAW
// =======================================================
void ssd1306_draw_pixel(ssd1306_t *lcd,
                        uint8_t x,
                        uint8_t y,
                        bool color)
{
    if (!lcd || !lcd->initialized)
        return;

    if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT)
        return;

    uint16_t index = x + (y / 8) * SSD1306_WIDTH;

    if (color)
        lcd->buffer[index] |= (1 << (y % 8));
    else
        lcd->buffer[index] &= ~(1 << (y % 8));
}

// =======================================================
// LINE DRAW (BRESENHAM)
// =======================================================
void ssd1306_draw_line(ssd1306_t *lcd,
                       uint8_t x1,
                       uint8_t y1,
                       uint8_t x2,
                       uint8_t y2,
                       bool color)
{
    if (!lcd || !lcd->initialized)
        return;

    int dx = abs(x2 - x1);
    int sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1);
    int sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;

    while (1)
    {
        ssd1306_draw_pixel(lcd, x1, y1, color);

        if (x1 == x2 && y1 == y2)
            break;

        int e2 = 2 * err;

        if (e2 >= dy)
        {
            err += dy;
            x1 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y1 += sy;
        }
    }
}

// =======================================================
// DRAW CHAR (FONT 5x7)
// =======================================================
static void ssd1306_draw_char(ssd1306_t *lcd,
                              uint8_t x,
                              uint8_t y,
                              char c)
{
    if (!lcd || !lcd->font || !lcd->font->bitmap)
        return;

    uint8_t w = lcd->font->width;
    uint8_t h = lcd->font->height;

    const uint8_t *bitmap = &lcd->font->bitmap[(uint8_t)c * w];

    for (int col = 0; col < w; col++)
    {
        uint8_t line = bitmap[col];

        for (int row = 0; row < h; row++)
        {
            ssd1306_draw_pixel(lcd,
                               x + col,
                               y + row,
                               line & (1 << row));
        }
    }
}

// =======================================================
// DRAW STRING
// =======================================================
void ssd1306_draw_string(ssd1306_t *lcd,
                         uint8_t x,
                         uint8_t y,
                         const char *str)
{
    if (!lcd || !lcd->font || !str)
        return;

    uint8_t step = lcd->font->width + lcd->font->spacing;

    while (*str && x < SSD1306_WIDTH)
    {
        ssd1306_draw_char(lcd, x, y, *str++);
        x += step;
    }
}

void ssd1306_display_on(ssd1306_t *dev)
{
    // Envoie la commande 0xAF (Display ON)
    // Utilise la fonction interne de ton driver pour écrire une commande
    // (Cherche dans le .c comment ssd1306_clear écrit sur le bus,
    // c'est souvent une fonction interne comme 'ssd1306_write_command')
    extern void ssd1306_write_command(ssd1306_t * dev, uint8_t command); // Déclaration interne
    ssd1306_write_cmd(dev, 0xAF);
    // 0x8D: Charge pump, 0x14: Enable, 0xAF: ON
    ssd1306_write_cmd(dev, 0x8D);
    ssd1306_write_cmd(dev, 0x14);
    ssd1306_write_cmd(dev, 0xAF);
    ssd1306_write_cmd(dev, 0xC8);
    ssd1306_write_cmd(dev,0xA1 );
}
