// display_ui.c
// Рабочая версия UI + SSD1306 basic text rendering (5x7 font) over I2C for ESP32.

#include "display_ui.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "webserver.h"
#include "attack.h"
#include "wifi_controller.h"

static const char *TAG = "display_ui";

#define I2C_MASTER_SCL_IO 33
#define I2C_MASTER_SDA_IO 32
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 400000
#define SSD1306_ADDR 0x3C

#define BUTTON_UP_GPIO 21
#define BUTTON_MIDDLE_GPIO 19
#define BUTTON_DOWN_GPIO 18

#define DEBUG_GREEN_GPIO 17
#define DEBUG_RED_GPIO 5

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define DISPLAY_PAGES (DISPLAY_HEIGHT/8)

static bool read_button_up(void){
    return gpio_get_level(BUTTON_UP_GPIO) == 1;
}
static bool read_button_middle(void){
    return gpio_get_level(BUTTON_MIDDLE_GPIO) == 1;
}
static bool read_button_down(void){
    return gpio_get_level(BUTTON_DOWN_GPIO) == 1;
}
static inline void debug_green(bool on){
    gpio_set_level(DEBUG_GREEN_GPIO, on ? 1 : 0);
}
static inline void debug_red(bool on){
    gpio_set_level(DEBUG_RED_GPIO, on ? 1 : 0);
}

static esp_err_t i2c_master_init(void){
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = {
            .clk_speed = I2C_MASTER_FREQ_HZ
        }
    };
    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
    if(ret != ESP_OK){
        ESP_LOGE(TAG,"i2c_param_config failed: %d", ret);
        return ret;
    }
    ret = i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if(ret != ESP_OK){
        ESP_LOGE(TAG,"i2c_driver_install failed: %d", ret);
    }
    return ret;
}


static void display_init(void) {
    uint8_t init_sequence[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
    };

    for (size_t i = 0; i < sizeof(init_sequence); i++) {
        uint8_t cmd = init_sequence[i];
        i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
        i2c_master_start(cmd_handle);
        i2c_master_write_byte(cmd_handle, (SSD1306_ADDR << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd_handle, 0x00, true);  // command
        i2c_master_write_byte(cmd_handle, cmd, true);
        i2c_master_stop(cmd_handle);
        i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_handle, pdMS_TO_TICKS(10));
        i2c_cmd_link_delete(cmd_handle);
    }
}

static uint8_t display_buffer[DISPLAY_WIDTH * DISPLAY_PAGES];

static void display_clear(void) {
    memset(display_buffer, 0x00, sizeof(display_buffer));
}

static void display_update(void) {
    for (uint8_t page = 0; page < DISPLAY_PAGES; page++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (SSD1306_ADDR << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, 0x00, true);
        i2c_master_write_byte(cmd, 0xB0 | page, true);
        i2c_master_write_byte(cmd, 0x00, true);
        i2c_master_write_byte(cmd, 0x10, true); 
        i2c_master_stop(cmd);
        i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(10));
        i2c_cmd_link_delete(cmd);

        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (SSD1306_ADDR << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, 0x40, true); 
        i2c_master_write(cmd, &display_buffer[page * DISPLAY_WIDTH], DISPLAY_WIDTH, true);
        i2c_master_stop(cmd);
        i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
    }
}


static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' ' 32
    {0x00,0x00,0x5F,0x00,0x00}, // '!' 33
    {0x00,0x07,0x00,0x07,0x00}, // '"' 34
    {0x14,0x7F,0x14,0x7F,0x14}, // '#' 35
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$' 36
    {0x23,0x13,0x08,0x64,0x62}, // '%' 37
    {0x36,0x49,0x55,0x22,0x50}, // '&' 38
    {0x00,0x05,0x03,0x00,0x00}, // ''' 39
    {0x00,0x1C,0x22,0x41,0x00}, // '(' 40
    {0x00,0x41,0x22,0x1C,0x00}, // ')' 41
    {0x14,0x08,0x3E,0x08,0x14}, // '*' 42
    {0x08,0x08,0x3E,0x08,0x08}, // '+' 43
    {0x00,0x50,0x30,0x00,0x00}, // ',' 44
    {0x08,0x08,0x08,0x08,0x08}, // '-' 45
    {0x00,0x60,0x60,0x00,0x00}, // '.' 46
    {0x20,0x10,0x08,0x04,0x02}, // '/' 47
    {0x3E,0x51,0x49,0x45,0x3E}, // '0' 48
    {0x00,0x42,0x7F,0x40,0x00}, // '1' 49
    {0x42,0x61,0x51,0x49,0x46}, // '2' 50
    {0x21,0x41,0x45,0x4B,0x31}, // '3' 51
    {0x18,0x14,0x12,0x7F,0x10}, // '4' 52
    {0x27,0x45,0x45,0x45,0x39}, // '5' 53
    {0x3C,0x4A,0x49,0x49,0x30}, // '6' 54
    {0x01,0x71,0x09,0x05,0x03}, // '7' 55
    {0x36,0x49,0x49,0x49,0x36}, // '8' 56
    {0x06,0x49,0x49,0x29,0x1E}, // '9' 57
    {0x00,0x36,0x36,0x00,0x00}, // ':' 58
    {0x00,0x56,0x36,0x00,0x00}, // ';' 59
    {0x08,0x14,0x22,0x41,0x00}, // '<' 60
    {0x14,0x14,0x14,0x14,0x14}, // '=' 61
    {0x00,0x41,0x22,0x14,0x08}, // '>' 62
    {0x02,0x01,0x51,0x09,0x06}, // '?' 63
    {0x32,0x49,0x79,0x41,0x3E}, // '@' 64
    {0x7E,0x11,0x11,0x11,0x7E}, // 'A' 65
    {0x7F,0x49,0x49,0x49,0x36}, // 'B' 66
    {0x3E,0x41,0x41,0x41,0x22}, // 'C' 67
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D' 68
    {0x7F,0x49,0x49,0x49,0x41}, // 'E' 69
    {0x7F,0x09,0x09,0x09,0x01}, // 'F' 70
    {0x3E,0x41,0x49,0x49,0x7A}, // 'G' 71
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H' 72
    {0x00,0x41,0x7F,0x41,0x00}, // 'I' 73
    {0x20,0x40,0x41,0x3F,0x01}, // 'J' 74
    {0x7F,0x08,0x14,0x22,0x41}, // 'K' 75
    {0x7F,0x40,0x40,0x40,0x40}, // 'L' 76
    {0x7F,0x02,0x04,0x02,0x7F}, // 'M' 77
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N' 78
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O' 79
    {0x7F,0x09,0x09,0x09,0x06}, // 'P' 80
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q' 81
    {0x7F,0x09,0x19,0x29,0x46}, // 'R' 82
    {0x46,0x49,0x49,0x49,0x31}, // 'S' 83
    {0x01,0x01,0x7F,0x01,0x01}, // 'T' 84
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U' 85
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V' 86
    {0x3F,0x40,0x38,0x40,0x3F}, // 'W' 87
    {0x63,0x14,0x08,0x14,0x63}, // 'X' 88
    {0x07,0x08,0x70,0x08,0x07}, // 'Y' 89
    {0x61,0x51,0x49,0x45,0x43}, // 'Z' 90
    {0x00,0x7F,0x41,0x41,0x00}, // '[' 91
    {0x02,0x04,0x08,0x10,0x20}, // '\' 92
    {0x00,0x41,0x41,0x7F,0x00}, // ']' 93
    {0x04,0x02,0x01,0x02,0x04}, // '^' 94
    {0x40,0x40,0x40,0x40,0x40}, // '_' 95
    {0x00,0x01,0x02,0x04,0x00}, // '`' 96
    {0x20,0x54,0x54,0x54,0x78}, // 'a' 97
    {0x7F,0x48,0x44,0x44,0x38}, // 'b' 98
    {0x38,0x44,0x44,0x44,0x20}, // 'c' 99
    {0x38,0x44,0x44,0x48,0x7F}, // 'd'100
    {0x38,0x54,0x54,0x54,0x18}, // 'e'101
    {0x08,0x7E,0x09,0x01,0x02}, // 'f'102
    {0x0C,0x52,0x52,0x52,0x3E}, // 'g'103
    {0x7F,0x08,0x04,0x04,0x78}, // 'h'104
    {0x00,0x44,0x7D,0x40,0x00}, // 'i'105
    {0x20,0x40,0x44,0x3D,0x00}, // 'j'106
    {0x7F,0x10,0x28,0x44,0x00}, // 'k'107
    {0x00,0x41,0x7F,0x40,0x00}, // 'l'108
    {0x7C,0x04,0x18,0x04,0x78}, // 'm'109
    {0x7C,0x08,0x04,0x04,0x78}, // 'n'110
    {0x38,0x44,0x44,0x44,0x38}, // 'o'111
    {0x7C,0x14,0x14,0x14,0x08}, // 'p'112
    {0x08,0x14,0x14,0x18,0x7C}, // 'q'113
    {0x7C,0x08,0x04,0x04,0x08}, // 'r'114
    {0x48,0x54,0x54,0x54,0x20}, // 's'115
    {0x04,0x3F,0x44,0x40,0x20}, // 't'116
    {0x3C,0x40,0x40,0x20,0x7C}, // 'u'117
    {0x1C,0x20,0x40,0x20,0x1C}, // 'v'118
    {0x3C,0x40,0x30,0x40,0x3C}, // 'w'119
    {0x44,0x28,0x10,0x28,0x44}, // 'x'120
    {0x0C,0x50,0x50,0x50,0x3C}, // 'y'121
    {0x44,0x64,0x54,0x4C,0x44}, // 'z'122
    {0x00,0x08,0x36,0x41,0x00}, // '{'123
    {0x00,0x00,0x7F,0x00,0x00}, // '|'124
    {0x00,0x41,0x36,0x08,0x00}, // '}'125
    {0x10,0x08,0x08,0x10,0x08}, // '~'126
    {0x00,0x06,0x09,0x09,0x06}  // 127 (DEL-like)
};


static int draw_char(int x, int y, char c) {
    if (c < 32 || c > 127) c = '?';
    const uint8_t *ch = font5x7[c - 32];
    int page = y / 8;
    if (page < 0 || page >= DISPLAY_PAGES) return 0;
    if (x < 0 || x >= DISPLAY_WIDTH) return 0;
    for (int col = 0; col < 5; col++) {
        if (x + col >= DISPLAY_WIDTH) break;
        uint8_t col_byte = ch[col]; 
        display_buffer[page * DISPLAY_WIDTH + (x + col)] = col_byte;
    }
    if (x + 5 < DISPLAY_WIDTH) {
        display_buffer[page * DISPLAY_WIDTH + (x + 5)] = 0x00;
    }
    return 6;
}

static void display_text(int x, int y, const char *text) {
    if (!text) return;
    if (y % 8 != 0) {
        ESP_LOGW(TAG,"display_text: y must be multiple of 8 (page-aligned). y=%d", y);
        return;
    }
    int curx = x;
    while (*text && curx < DISPLAY_WIDTH) {
        int adv = draw_char(curx, y, *text);
        curx += adv;
        text++;
    }
}

static void display_print_lines(const char *l1, const char *l2) {
    display_clear();
    display_text(0, 0, l1 ? l1 : "");
    display_text(0, 32, l2 ? l2 : "");  // page 4 (32px)
    display_update();
}
static TickType_t ui_attack_start_tick = 0;
static uint8_t ui_attack_timeout = 0;
static bool ui_triggered_attack = false;

static void post_attack_request(uint8_t ap_id, uint8_t type, uint8_t method, uint8_t timeout){
    attack_request_t req = { .ap_record_id = ap_id, .type = type, .method = method, .timeout = timeout };
    ESP_LOGI(TAG, "Posting attack request: ap=%u type=%u method=%u timeout=%u", ap_id, type, method, timeout);
    ESP_ERROR_CHECK(esp_event_post(WEBSERVER_EVENTS, WEBSERVER_EVENT_ATTACK_REQUEST, &req, sizeof(req), portMAX_DELAY));
    ui_attack_start_tick = xTaskGetTickCount();
    ui_attack_timeout = timeout;
    ui_triggered_attack = true;
}

static void display_ui_task(void *arg){
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL<<BUTTON_UP_GPIO) | (1ULL<<BUTTON_MIDDLE_GPIO) | (1ULL<<BUTTON_DOWN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_cfg);

    gpio_config_t dbg_cfg = {
        .pin_bit_mask = (1ULL<<DEBUG_GREEN_GPIO) | (1ULL<<DEBUG_RED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&dbg_cfg);
    gpio_set_level(DEBUG_GREEN_GPIO, 0);
    gpio_set_level(DEBUG_RED_GPIO, 0);

    if(i2c_master_init() != ESP_OK){
        ESP_LOGW(TAG, "I2C init failed - Display output will be in serial log only");
        debug_red(true);
    } else {
        display_init();
        display_clear();
        display_update();
        debug_red(false);
        debug_green(true);
        vTaskDelay(pdMS_TO_TICKS(200));
        debug_green(false);
    }

    wifictl_scan_nearby_aps();
    const wifictl_ap_records_t *records = wifictl_get_ap_records();
    unsigned ap_count = (records) ? records->count : 0;

    uint8_t selected_ap = 0;
    uint8_t selected_type = 1;
    uint8_t selected_method = 0;
    uint8_t timeout = 30;

    const attack_status_t *attack_status = NULL;

    const char *attack_type_names[] = { "PASSIVE", "HANDSHAKE", "PMKID", "DOS" };
    const char *handshake_methods[] = { "ROGUE_AP", "BROADCAST", "PASSIVE" };
    const char *dos_methods[] = { "ROGUE_AP", "BROADCAST", "COMBINE" };

    enum { MENU_AP, MENU_TYPE, MENU_METHOD, MENU_TIMEOUT, MENU_START } menu = MENU_AP;

    while(1){
        attack_status = attack_get_status();
        char line1[33]={0}, line2[33]={0};
        if(ui_triggered_attack && attack_status && attack_status->state == RUNNING){
            TickType_t now = xTaskGetTickCount();
            uint32_t elapsed = (now - ui_attack_start_tick) / configTICK_RATE_HZ;
            int remaining = (int)ui_attack_timeout - (int)elapsed;
            if(remaining < 0) remaining = 0;
            snprintf(line1, sizeof(line1), "ATTACK %s", attack_type_names[selected_type]);
            snprintf(line2, sizeof(line2), "Left: %3us", remaining);
            display_print_lines(line1,line2);
            if(read_button_middle()){
                debug_green(true);
                vTaskDelay(pdMS_TO_TICKS(100));
                debug_green(false);

                vTaskDelay(pdMS_TO_TICKS(50));
                menu = MENU_AP;
                ui_triggered_attack = false;
                while(read_button_middle()) vTaskDelay(pdMS_TO_TICKS(20));
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if(ui_triggered_attack && attack_status && attack_status->state != RUNNING){
            snprintf(line1, sizeof(line1), "Attack finished");
            snprintf(line2, sizeof(line2), "Press to menu");
            display_print_lines(line1,line2);
            if(read_button_middle()){
                debug_green(false);
                debug_red(true);
                vTaskDelay(pdMS_TO_TICKS(150));
                debug_red(false);

                vTaskDelay(pdMS_TO_TICKS(50));
                menu = MENU_AP;
                ui_triggered_attack = false;
                while(read_button_middle()) vTaskDelay(pdMS_TO_TICKS(20));
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        switch(menu){
            case MENU_AP:
                snprintf(line1, sizeof(line1), "AP %u/%u", selected_ap+1, ap_count);
                if(ap_count>0){
                    const wifi_ap_record_t *r = wifictl_get_ap_record(selected_ap);
                    char ssid[33]; memset(ssid,0,sizeof(ssid)); memcpy(ssid,r->ssid,32);
                    snprintf(line2, sizeof(line2), "%.*s", (int)(sizeof(line2)-1), ssid);
                } else snprintf(line2,sizeof(line2),"No APs found");
                break;
            case MENU_TYPE:
                snprintf(line1,sizeof(line1),"Type: %s", attack_type_names[selected_type]);
                snprintf(line2,sizeof(line2),"Use joystick");
                break;
            case MENU_METHOD:
                if(selected_type == ATTACK_TYPE_HANDSHAKE){
                    snprintf(line1,sizeof(line1),"Method: %s", handshake_methods[selected_method % 3]);
                } else if(selected_type == ATTACK_TYPE_DOS){
                    snprintf(line1,sizeof(line1),"Method: %s", dos_methods[selected_method % 3]);
                } else {
                    snprintf(line1,sizeof(line1),"Method: %u", selected_method);
                }
                snprintf(line2,sizeof(line2),"(press to next)");
                break;
            case MENU_TIMEOUT:
                snprintf(line1,sizeof(line1),"Timeout: %us", timeout);
                snprintf(line2,sizeof(line2),"Press to edit");
                break;
            case MENU_START:
                snprintf(line1,sizeof(line1),"Start");
                snprintf(line2,sizeof(line2),"Press to confirm");
                break;
        }
        display_print_lines(line1,line2);

        if(read_button_up()){
            if(menu==MENU_AP && ap_count>0){ if(selected_ap>0) selected_ap--; }
            else if(menu==MENU_TYPE){ if(selected_type>0) selected_type--; }
            else if(menu==MENU_METHOD){ if(selected_type==ATTACK_TYPE_HANDSHAKE){ if(selected_method>0) selected_method--; } else if(selected_type==ATTACK_TYPE_DOS){ if(selected_method>0) selected_method--; } else { if(selected_method>0) selected_method--; } }
            else if(menu==MENU_TIMEOUT){ if(timeout>5) timeout-=5; }
            while(read_button_up()) vTaskDelay(pdMS_TO_TICKS(20));
            debug_red(true);
            vTaskDelay(pdMS_TO_TICKS(50));
            debug_red(false);
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if(read_button_down()){
            if(menu==MENU_AP && ap_count>0){ if(selected_ap+1<ap_count) selected_ap++; }
            else if(menu==MENU_TYPE){ if(selected_type<3) selected_type++; }
            else if(menu==MENU_METHOD){ if(selected_type==ATTACK_TYPE_HANDSHAKE){ selected_method = (selected_method+1) % 3; } else if(selected_type==ATTACK_TYPE_DOS){ selected_method = (selected_method+1) % 3; } else { selected_method++; } }
            else if(menu==MENU_TIMEOUT){ timeout+=5; }
            while(read_button_down()) vTaskDelay(pdMS_TO_TICKS(20));
            debug_red(true);
            vTaskDelay(pdMS_TO_TICKS(50));
            debug_red(false);
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if(read_button_middle()){
            debug_green(true);
            vTaskDelay(pdMS_TO_TICKS(100));
            debug_green(false);

            vTaskDelay(pdMS_TO_TICKS(50));
            if(menu==MENU_START){
                post_attack_request(selected_ap, selected_type, selected_method, timeout);
                debug_green(true);
            } else {
                menu = (menu + 1) % 5;
            }
            while(read_button_middle()) vTaskDelay(pdMS_TO_TICKS(20));
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void display_ui_start(void){
    xTaskCreate(display_ui_task, "display_ui", 8192, NULL, 5, NULL);
}
