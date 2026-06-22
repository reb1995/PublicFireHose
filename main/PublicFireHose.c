#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

#ifdef CONFIG_IDF_TARGET_ESP32C6
    #include "driver/gpio.h"
    #define RF_SW_PWR_PIN 3    // Power for RF Switch
    #define RF_ANT_SEL_PIN 14  // External Antenna Select
#endif


static const char *TAG = "NOISE_GENERATOR";

const uint8_t beacon_base_hdr[36] = {
    0x80, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x64, 0x00,
    0x01, 0x00
};

const uint8_t probe_base_hdr[24] = {
    0x40, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00
};

const uint8_t supported_rates[] = {0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};

#ifdef CONFIG_IDF_TARGET_ESP32C5
const uint8_t hopping_sequence[] = {1, 6, 11, 36, 40, 44, 48, 149, 153, 157, 161, 165};
#else
const uint8_t hopping_sequence[] = {1, 6, 11};
#endif

#define SEQUENCE_LEN (sizeof(hopping_sequence) / sizeof(hopping_sequence[0]))

void wifi_noise_task(void *pvParameters) {
    uint8_t packet_buffer[256];
    char rand_ssid[33];

    // Start index at the very end of the array
    int chan_idx = SEQUENCE_LEN - 1;

    while (1) {
        // --- 1. CONFIGURE THE GHOST DEVICE ---
        uint8_t current_channel = hopping_sequence[chan_idx];
        if (chan_idx == 0) {
            chan_idx = SEQUENCE_LEN - 1;
        } else {
            chan_idx--;
        }
        esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);

        size_t ssid_len = (esp_random() % 32) + 1;
        for (size_t i = 0; i < ssid_len; i++) {
            rand_ssid[i] = (char)((esp_random() % 94) + 33);
        }
        rand_ssid[ssid_len] = '\0';

        uint8_t mac[6];
        mac[0] = (esp_random() & 0xFC) | 0x02;
        mac[1] = esp_random() & 0xFF;
        mac[2] = esp_random() & 0xFF;
        mac[3] = esp_random() & 0xFF;
        mac[4] = esp_random() & 0xFF;
        mac[5] = esp_random() & 0xFF;

        size_t offset = 0;
        bool is_beacon = (esp_random() % 2 == 0); // 50/50 Coin Flip

        // --- 2. BUILD THE PAYLOAD (Done once per burst) ---
        if (is_beacon) {
            memcpy(packet_buffer + offset, beacon_base_hdr, sizeof(beacon_base_hdr));
            memcpy(&packet_buffer[10], mac, 6);
            memcpy(&packet_buffer[16], mac, 6);
            offset += sizeof(beacon_base_hdr);

            packet_buffer[offset++] = 0x00;
            packet_buffer[offset++] = ssid_len;
            memcpy(packet_buffer + offset, rand_ssid, ssid_len);
            offset += ssid_len;

            memcpy(packet_buffer + offset, supported_rates, sizeof(supported_rates));
            offset += sizeof(supported_rates);

            packet_buffer[offset++] = 0x03;
            packet_buffer[offset++] = 0x01;
            packet_buffer[offset++] = current_channel;
        } else {
            memcpy(packet_buffer + offset, probe_base_hdr, sizeof(probe_base_hdr));
            memcpy(&packet_buffer[10], mac, 6);
            offset += sizeof(probe_base_hdr);

            packet_buffer[offset++] = 0x00;
            packet_buffer[offset++] = ssid_len;
            memcpy(packet_buffer + offset, rand_ssid, ssid_len);
            offset += ssid_len;

            memcpy(packet_buffer + offset, supported_rates, sizeof(supported_rates));
            offset += sizeof(supported_rates);
        }

        ESP_LOGI(TAG, "Spawning %s Ch: %03d | MAC: %02x:%02x:%02x:%02x:%02x:%02x | SSID: %s",
                 is_beacon ? "[ROUTER]" : "[CLIENT]",
                 current_channel, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rand_ssid);

        // --- 3. THE BURST LOOP (Fire 20 frames) ---
        for (int i = 0; i < 20; i++) {
            // Increment the Wi-Fi Sequence Number for added realism
            // Byte 22 is the lower 8 bits of the sequence control field
            packet_buffer[22] = (i << 4) & 0xFF;
            packet_buffer[23] = 0x00;

            esp_wifi_80211_tx(WIFI_IF_STA, packet_buffer, offset, true);

            // Wait 20ms between frames in the burst (50 packets per second rate)
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        // --- 4. THE LONG WAIT ---
        // Sleep for 100 ms before moving to the next channel and generating a new device
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void ble_noise_task(void *pvParameters) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    char rand_name[20];

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    while (1) {
        ble_gap_adv_stop();

        uint8_t mac[6];
        for (int i = 0; i < 6; i++) {
            mac[i] = esp_random() & 0xFF;
        }
        mac[5] &= 0x3F;
        ble_hs_id_set_rnd(mac);

        size_t name_len = (esp_random() % 15) + 4;
        for (size_t i = 0; i < name_len; i++) {
            rand_name[i] = (char)((esp_random() % 26) + 'A');
        }
        rand_name[name_len] = '\0';

        memset(&fields, 0, sizeof(fields));
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        fields.name = (uint8_t *)rand_name;
        fields.name_len = name_len;
        fields.name_is_complete = 1;

        ble_gap_adv_set_fields(&fields);
        ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL);

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void ble_on_sync(void) {
    xTaskCreate(ble_noise_task, "ble_noise", 4096, NULL, 5, NULL);
}

void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void) {

#ifdef CONFIG_IDF_TARGET_ESP32C6
    // Enable the C6 External Antenna
    gpio_reset_pin(RF_SW_PWR_PIN);
    gpio_set_direction(RF_SW_PWR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RF_SW_PWR_PIN, 0);

    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_reset_pin(RF_ANT_SEL_PIN);
    gpio_set_direction(RF_ANT_SEL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RF_ANT_SEL_PIN, 1);
#endif

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    esp_wifi_start();

    xTaskCreate(wifi_noise_task, "wifi_noise", 4096, NULL, 5, NULL);

    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);
}
