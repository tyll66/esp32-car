#include "lcd_driver.h"

static const char *TAG = "LCD_LVGL";

typedef enum {
    LCD_UI_MSG_TEXT = 0,
    LCD_UI_MSG_IMAGE
} lcd_ui_msg_type_t;

typedef struct {
    lcd_ui_msg_type_t type;

    char text[32];

    /*
     * 指向LVGL生成的图片描述符。
     * 图片数据必须是全局/static数据，不能是局部变量。
     */
    const lv_image_dsc_t *image;
} lcd_status_msg_t;

static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_panel_io_handle_t s_lcd_io = NULL;

static lv_disp_t *s_disp = NULL;

/* 原来的文字对象 */
static lv_obj_t *s_status_label = NULL;

/* 新增的图片对象 */
static lv_obj_t *s_status_image = NULL;

static QueueHandle_t s_status_queue = NULL;

static void lcd_backlight_init(void)
{
    gpio_config_t bl_conf = {
        .pin_bit_mask = (1ULL << LCD_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&bl_conf));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, 1));

    ESP_LOGI(TAG, "LCD backlight ON");
}

static esp_err_t lcd_panel_init(void)
{
    ESP_LOGI(TAG, "Initialize SPI LCD panel");

    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        //单次最大传输时长
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
    };

    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
        TAG,
        "spi_bus_initialize failed"
    );

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
            &io_config,
            &s_lcd_io
        ),
        TAG,
        "esp_lcd_new_panel_io_spi failed"
    );

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7789(s_lcd_io, &panel_config, &s_lcd_panel),
        TAG,
        "esp_lcd_new_panel_st7789 failed"
    );

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), TAG, "panel init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_lcd_panel, true), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_lcd_panel, false, false), TAG, "mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_lcd_panel, false), TAG, "swap xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_lcd_panel, true), TAG, "display on failed");

    ESP_LOGI(TAG, "LCD panel init done");

    return ESP_OK;
}

static esp_err_t lcd_lvgl_init(void)
{
    ESP_LOGI(TAG, "Initialize LVGL port");

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();

    ESP_RETURN_ON_ERROR(
        lvgl_port_init(&lvgl_cfg),
        TAG,
        "lvgl_port_init failed"
    );

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_lcd_panel,
        .buffer_size = LCD_H_RES * 40,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = false,
        },
    };

    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LVGL display added");

    return ESP_OK;
}

static void lcd_create_status_ui(void)
{
    /*
     * esp_lvgl_port要求：
     * 所有LVGL对象操作必须加锁。
     */
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_disp_get_scr_act(s_disp);

    lv_obj_clean(scr);

    /*
     * 黑色背景。
     */
    lv_obj_set_style_bg_color(
        scr,
        lv_color_hex(0x000000),
        LV_PART_MAIN
    );

    lv_obj_set_style_bg_opa(
        scr,
        LV_OPA_COVER,
        LV_PART_MAIN
    );

    /*
     * 创建原来的文字对象。
     */
    s_status_label = lv_label_create(scr);

    lv_label_set_text(
        s_status_label,
        "BOOT"
    );

    lv_obj_set_style_text_color(
        s_status_label,
        lv_color_hex(0x00FF80),
        LV_PART_MAIN
    );

    lv_obj_set_style_text_font(
        s_status_label,
        &lv_font_montserrat_28,
        LV_PART_MAIN
    );

    lv_obj_set_style_text_align(
        s_status_label,
        LV_TEXT_ALIGN_CENTER,
        LV_PART_MAIN
    );

    lv_obj_set_width(
        s_status_label,
        LCD_H_RES - 10
    );

    lv_obj_center(s_status_label);

    /*
     * 创建图片对象。
     *
     * 此时先不设置图片源，并默认隐藏。
     */
    s_status_image = lv_image_create(scr);

    lv_obj_center(s_status_image);

    lv_obj_add_flag(
        s_status_image,
        LV_OBJ_FLAG_HIDDEN
    );

    lvgl_port_unlock();
}

void lcd_status_set_direct(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        text = "READY";
    }

    if (s_status_label == NULL) {
        return;
    }

    lvgl_port_lock(0);

    /*
     * 显示文字时隐藏图片。
     */
    if (s_status_image != NULL) {
        lv_obj_add_flag(
            s_status_image,
            LV_OBJ_FLAG_HIDDEN
        );
    }

    /*
     * 显示文字对象。
     */
    lv_obj_clear_flag(
        s_status_label,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_label_set_text(
        s_status_label,
        text
    );

    lv_obj_center(s_status_label);

    lvgl_port_unlock();
}

void lcd_image_set_direct(const lv_image_dsc_t *image)
{
    if (image == NULL) {
        ESP_LOGW(TAG, "lcd_image_set_direct: image is NULL");
        return;
    }

    if (s_status_image == NULL) {
        ESP_LOGW(TAG, "lcd_image_set_direct: image object not ready");
        return;
    }

    lvgl_port_lock(0);

    /*
     * 显示图片时隐藏文字。
     */
    if (s_status_label != NULL) {
        lv_obj_add_flag(
            s_status_label,
            LV_OBJ_FLAG_HIDDEN
        );
    }

    /*
     * 设置图片数据源。
     */
    lv_image_set_src(
        s_status_image,
        image
    );

    /*
     * 图片居中。
     */
    lv_obj_center(s_status_image);

    /*
     * 显示图片对象。
     */
    lv_obj_clear_flag(
        s_status_image,
        LV_OBJ_FLAG_HIDDEN
    );

    lvgl_port_unlock();
}

static void lcd_status_task(void *arg)
{
    (void)arg;

    lcd_status_msg_t msg;

    while (1) {
        if (xQueueReceive(
                s_status_queue,
                &msg,
                portMAX_DELAY
            ) != pdTRUE) {
            continue;
        }

        switch (msg.type) {
            case LCD_UI_MSG_TEXT:
                lcd_status_set_direct(msg.text);
                break;

            case LCD_UI_MSG_IMAGE:
                lcd_image_set_direct(msg.image);
                break;

            default:
                ESP_LOGW(
                    TAG,
                    "Unknown LCD message type: %d",
                    (int)msg.type
                );
                break;
        }
    }
}

esp_err_t lcd_driver_init(void)
{
    lcd_backlight_init();

    ESP_RETURN_ON_ERROR(lcd_panel_init(), TAG, "lcd_panel_init failed");
    ESP_RETURN_ON_ERROR(lcd_lvgl_init(), TAG, "lcd_lvgl_init failed");

    lcd_create_status_ui();

    return ESP_OK;
}

void lcd_status_post(const char *text)
{
    if (s_status_queue == NULL || text == NULL) {
        return;
    }

    lcd_status_msg_t msg = {
        .type = LCD_UI_MSG_TEXT,
        .image = NULL,
    };

    strncpy(
        msg.text,
        text,
        sizeof(msg.text) - 1
    );

    msg.text[sizeof(msg.text) - 1] = '\0';

    xQueueOverwrite(
        s_status_queue,
        &msg
    );
}


void lcd_image_post(const lv_image_dsc_t *image)
{
    if (s_status_queue == NULL) {
        ESP_LOGW(TAG, "lcd_image_post: status queue not started");
        return;
    }

    if (image == NULL) {
        ESP_LOGW(TAG, "lcd_image_post: image is NULL");
        return;
    }

    lcd_status_msg_t msg = {
        .type = LCD_UI_MSG_IMAGE,
        .image = image,
    };

    /*
     * 队列长度为1，因此使用xQueueOverwrite，
     * 始终显示最新状态。
     */
    xQueueOverwrite(
        s_status_queue,
        &msg
    );
}

esp_err_t lcd_status_start(void)
{
    if (s_status_queue == NULL) {
        s_status_queue = xQueueCreate(1, sizeof(lcd_status_msg_t));
        if (s_status_queue == NULL) {
            ESP_LOGE(TAG, "status queue create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t ret = xTaskCreate(
        lcd_status_task,
        "lcd_status_task",
        4096,
        NULL,
        4,
        NULL
    );

    return ret == pdPASS ? ESP_OK : ESP_FAIL;
}

lv_disp_t *lcd_driver_get_disp(void)
{
    return s_disp;
}