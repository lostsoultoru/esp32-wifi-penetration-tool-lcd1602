/**
 * @file wifi_scan.c
 * @brief Scans nearby Wi-Fi APs using existing wifi_controller component and prints them to serial
 */
#include "wifi_scan.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "wifi_controller.h"

static const char *TAG = "wifi_scan";

void wifi_scan_print_serial(void){
    ESP_LOGI(TAG, "Starting Wi-Fi scan and printing results...");

    // Trigger a blocking scan (the ap_scanner uses esp_wifi_scan_start with block=true)
    wifictl_scan_nearby_aps();

    const wifictl_ap_records_t *records = wifictl_get_ap_records();
    if(records == NULL){
        ESP_LOGE(TAG, "No scan results available");
        return;
    }

    ESP_LOGI(TAG, "--- Scan results: %u AP(s) ---", records->count);
    for(unsigned i = 0; i < records->count; ++i){
        const wifi_ap_record_t *r = wifictl_get_ap_record(i);
        if(r == NULL) continue;

        // Make sure SSID is null-terminated
        char ssid[33];
        memset(ssid, 0, sizeof(ssid));
        memcpy(ssid, r->ssid, sizeof(r->ssid));

        ESP_LOGI(TAG, "%u: SSID='%s' RSSI=%d CH=%d AUTH=%d BSSID=%02x:%02x:%02x:%02x:%02x:%02x",
                 i+1, ssid, r->rssi, r->primary, r->authmode,
                 r->bssid[0], r->bssid[1], r->bssid[2], r->bssid[3], r->bssid[4], r->bssid[5]);
    }
}
