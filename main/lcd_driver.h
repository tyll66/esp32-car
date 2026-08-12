#pragma once

#include "esp_err.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#include "esp_lvgl_port.h"

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lcd_driver_init(void);
esp_err_t lcd_status_start(void);

void lcd_status_post(const char *text);
void lcd_status_set_direct(const char *text);

void lcd_image_set_direct(
    const lv_image_dsc_t *image
);

void lcd_image_post(
    const lv_image_dsc_t *image
);

lv_disp_t *lcd_driver_get_disp(void);

#ifdef __cplusplus
}
#endif