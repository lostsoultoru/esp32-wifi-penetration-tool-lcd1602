/**
 * @file lcd_ui.c
 * @brief Minimal UI: LCD1602 (I2C PCF8574) + joystick to select AP/type/method/timeout and start attack
 *
 * Hardware assumptions (common):
 * - LCD1602 connected via I2C PCF8574 backpack at address 0x27 (CONFIGURABLE)
 * - Joystick: X -> ADC1_CHANNEL_0 (GPIO36), Y -> ADC1_CHANNEL_3 (GPIO39), Button -> GPIO34
 *
 * This is a minimal, tolerant implementation intended to be adapted to your hardware pins.
 */

#include "lcd_ui.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c.h"
#include "driver/adc.h"
#include "webserver.h"
#include "attack.h"
#include "wifi_controller.h"

static const char *TAG = "lcd_ui";

#define I2C_MASTER_SCL_IO 33
#define I2C_MASTER_SDA_IO 32
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 100000
#define PCF8574_ADDR 0x27

#define BUTTON_UP_GPIO 21
#define BUTTON_MIDDLE_GPIO 19
#define BUTTON_DOWN_GPIO 18

#define DEBUG_GREEN_GPIO 17
#define DEBUG_RED_GPIO 5

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
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
    if(ret != ESP_OK) return ret;
    return i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0);
}

#define LCD_BACKLIGHT 0x08
#define LCD_ENABLE    0x04
#define LCD_RW        0x02
#define LCD_RS        0x01

static esp_err_t pcf_write(uint8_t data){
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF8574_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void lcd_pulse_enable(uint8_t data){
    pcf_write(data | LCD_ENABLE);
    ets_delay_us(1);
    pcf_write(data & ~LCD_ENABLE);
    ets_delay_us(50);
}

static void lcd_write4(uint8_t nibble, uint8_t flags){
    uint8_t data = (nibble & 0x0F) << 4;
    data |= flags & (LCD_BACKLIGHT | LCD_RS);
    pcf_write(data);
    lcd_pulse_enable(data);
}

static void lcd_write8(uint8_t val, uint8_t flags){
    lcd_write4((val >> 4) & 0x0F, flags);
    lcd_write4(val & 0x0F, flags);
}

static void lcd_command(uint8_t cmd){
    lcd_write8(cmd, LCD_BACKLIGHT);
}

static void lcd_data(uint8_t d){
    lcd_write8(d, LCD_BACKLIGHT | LCD_RS);
}

static void lcd_init_display(void){
    ets_delay_us(50000);
    lcd_write4(0x03, LCD_BACKLIGHT);
    ets_delay_us(4500);
    lcd_write4(0x03, LCD_BACKLIGHT);
    ets_delay_us(4500);
    lcd_write4(0x03, LCD_BACKLIGHT);
    ets_delay_us(150);
    lcd_write4(0x02, LCD_BACKLIGHT);
    lcd_command(0x28);
    lcd_command(0x0C);
    lcd_command(0x06);
    lcd_command(0x01);
    ets_delay_us(2000);
}

static void lcd_set_cursor(uint8_t col, uint8_t row){
    const uint8_t row_offsets[] = { 0x00, 0x40 };
    lcd_command(0x80 | (col + row_offsets[row]));
}

static void lcd_print_lines(const char *l1, const char *l2){
    lcd_set_cursor(0,0);
    for(int i=0;i<16;i++){
        char c = l1[i];
        if(c==0) c=' ';
        lcd_data((uint8_t)c);
    }
    lcd_set_cursor(0,1);
    for(int i=0;i<16;i++){
        char c = l2[i];
        if(c==0) c=' ';
        lcd_data((uint8_t)c);
    }
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

static void lcd_ui_task(void *arg){
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL<<BUTTON_UP_GPIO) | (1ULL<<BUTTON_MIDDLE_GPIO) | (1ULL<<BUTTON_DOWN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE
    };
    gpio_config(&btn_cfg);

    /* configure debug LED pins as outputs and default them off */
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
        ESP_LOGW(TAG, "I2C init failed - LCD output will be in serial log only");
        /* turn on red debug LED to indicate I2C/init error */
        debug_red(true);
    } else {
        lcd_init_display();
        /* brief green blink to indicate successful init */
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
        char line1[17]={0}, line2[17]={0};
        if(ui_triggered_attack && attack_status && attack_status->state == RUNNING){
            TickType_t now = xTaskGetTickCount();
            uint32_t elapsed = (now - ui_attack_start_tick) / configTICK_RATE_HZ;
            int remaining = (int)ui_attack_timeout - (int)elapsed;
            if(remaining < 0) remaining = 0;
            snprintf(line1, sizeof(line1), "ATTACK %s", attack_type_names[selected_type]);
            snprintf(line2, sizeof(line2), "Left: %3us", remaining);
            lcd_print_lines(line1,line2);
            if(read_button_middle()){
                /* indicate selection with green blink */
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
            lcd_print_lines(line1,line2);
            if(read_button_middle()){
                /* attack finished: turn off green, blink red */
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
        lcd_print_lines(line1,line2);
        
            if(read_button_up()){
            if(menu==MENU_AP && ap_count>0){ if(selected_ap>0) selected_ap--; }
            else if(menu==MENU_TYPE){ if(selected_type>0) selected_type--; }
            else if(menu==MENU_METHOD){ if(selected_type==ATTACK_TYPE_HANDSHAKE){ if(selected_method>0) selected_method--; } else if(selected_type==ATTACK_TYPE_DOS){ if(selected_method>0) selected_method--; } else { if(selected_method>0) selected_method--; } }
            else if(menu==MENU_TIMEOUT){ if(timeout>5) timeout-=5; }
            while(read_button_up()) vTaskDelay(pdMS_TO_TICKS(20));
                /* small red blink to indicate up press */
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
            /* small red blink to indicate down press */
            debug_red(true);
            vTaskDelay(pdMS_TO_TICKS(50));
            debug_red(false);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        
        if(read_button_middle()){
            /* indicate selection with green blink */
            debug_green(true);
            vTaskDelay(pdMS_TO_TICKS(100));
            debug_green(false);

            vTaskDelay(pdMS_TO_TICKS(50));
            if(menu==MENU_START){
                post_attack_request(selected_ap, selected_type, selected_method, timeout);
                /* indicate attack start */
                debug_green(true);
            } else {
                menu = (menu + 1) % 5;
            }
            while(read_button_middle()) vTaskDelay(pdMS_TO_TICKS(20));
        }
        
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void lcd_ui_start(void){
    xTaskCreate(lcd_ui_task, "lcd_ui", 4096, NULL, 5, NULL);
}
