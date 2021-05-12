#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#include "nvs_flash.h"
#include "esp_partition.h"

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_defs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "uECC.h"

#define CHECK_BIT(var,pos) ((var) & (1<<(7-pos)))

#define TEST_RTS (18)
#define TEST_CTS (18)

#define UART_PORT_NUM      (0)
#define UART_BAUD_RATE     (115200)
#define TASK_STACK_SIZE    (2048)

#define BUF_SIZE (1024)

// Set custom modem id before flashing:
static const uint32_t modem_id = 0x42424242;

static const char* LOG_TAG = "findmy_modem";

/** Callback function for BT events */
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

/** Random device address */
static esp_bd_addr_t rnd_addr = { 0xFF, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };

/** Advertisement payload */
static uint8_t adv_data[31] = {
    0x1e, /* Length (30) */
    0xff, /* Manufacturer Specific Data (type 0xff) */
    0x4c, 0x00, /* Company ID (Apple) */
    0x12, 0x19, /* Offline Finding type and length */
    0x00, /* State */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, /* First two bits */
    0x00, /* Hint (0x00) */
};

uint32_t swap_uint32( uint32_t val )
{
    val = ((val << 8) & 0xFF00FF00 ) | ((val >> 8) & 0xFF00FF );
    return (val << 16) | (val >> 16);
};

/* https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_gap_ble.html#_CPPv420esp_ble_adv_params_t */
static esp_ble_adv_params_t ble_adv_params = {
    // Advertising min interval:
    // Minimum advertising interval for undirected and low duty cycle
    // directed advertising. Range: 0x0020 to 0x4000 Default: N = 0x0800
    // (1.28 second) Time = N * 0.625 msec Time Range: 20 ms to 10.24 sec
    .adv_int_min        = 0x0640, 
    // Advertising max interval:
    // Maximum advertising interval for undirected and low duty cycle
    // directed advertising. Range: 0x0020 to 0x4000 Default: N = 0x0800
    // (1.28 second) Time = N * 0.625 msec Time Range: 20 ms to 10.24 sec
    .adv_int_max        = 0x0C80, 
    // Advertisement type
    .adv_type           = ADV_TYPE_NONCONN_IND,
    // Use the random address
    .own_addr_type      = BLE_ADDR_TYPE_RANDOM,
    // All channels
    .channel_map        = ADV_CHNL_ALL,
    // Allow both scan and connection requests from anyone. 
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    esp_err_t err;

    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            esp_ble_gap_start_advertising(&ble_adv_params);
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            // is it running?
            if ((err = param->adv_start_cmpl.status) != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(LOG_TAG, "advertising start failed: %s", esp_err_to_name(err));
            } else {
                ESP_LOGD(LOG_TAG, "advertising started");
            }
            break;

        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if ((err = param->adv_stop_cmpl.status) != ESP_BT_STATUS_SUCCESS){
                ESP_LOGE(LOG_TAG, "adv stop failed: %s", esp_err_to_name(err));
            }
            else {
                ESP_LOGD(LOG_TAG, "advertising stopped");
            }
            break;
        default:
            break;
    }
}

int is_valid_pubkey(uint8_t *pub_key_compressed) {
   uint8_t with_sign_byte[29];
   uint8_t pub_key_uncompressed[128];
   const struct uECC_Curve_t * curve = uECC_secp224r1();
   with_sign_byte[0] = 0x02;
   memcpy(&with_sign_byte[1], pub_key_compressed, 28);
   uECC_decompress(with_sign_byte, pub_key_uncompressed, curve);
   if(!uECC_valid_public_key(pub_key_uncompressed, curve)) {
       ESP_LOGW(LOG_TAG, "Generated public key tested as invalid");
       return 0;
   }
   return 1;
}

void pub_from_priv(uint8_t *pub_compressed, uint8_t *priv) {
   const struct uECC_Curve_t * curve = uECC_secp224r1();
   uint8_t pub_key_tmp[128];
   uECC_compute_public_key(priv, pub_key_tmp, curve);
   uECC_compress(pub_key_tmp, pub_compressed, curve);
}

void set_addr_from_key(esp_bd_addr_t addr, uint8_t *public_key) {
    addr[0] = public_key[0] | 0b11000000;
    addr[1] = public_key[1];
    addr[2] = public_key[2];
    addr[3] = public_key[3];
    addr[4] = public_key[4];
    addr[5] = public_key[5];
}

void set_payload_from_key(uint8_t *payload, uint8_t *public_key) {
    /* copy last 22 bytes */
    memcpy(&payload[7], &public_key[6], 22);
    /* append two bits of public key */
    payload[29] = public_key[0] >> 6;
}

void copy_4b_big_endian(uint8_t *dst, uint8_t *src) {
    dst[0] = src[3]; dst[1] = src[2]; dst[2] = src[1]; dst[3] = src[0];
}

// index as first part of payload to have an often changing MAC address
// [2b magic] [4byte index] [4byte msg_id] [4byte modem_id] [000.000] [1bit]
// There is a rade-off between sending and receiving throughput (e.g. we could also use a 1-byte lookup table)
void set_addr_and_payload_for_bit(uint32_t index, uint32_t msg_id, uint8_t bit) {
    uint32_t valid_key_counter = 0;
    static uint8_t public_key[28] = {0};
    public_key[0] = 0xBA; // magic value
    public_key[1] = 0xBE;
    copy_4b_big_endian(&public_key[2], &index);
    copy_4b_big_endian(&public_key[6], &msg_id);
    copy_4b_big_endian(&public_key[10], &modem_id);
    public_key[27] = bit;
    do {
        copy_4b_big_endian(&public_key[14], &valid_key_counter);
        // here, you could call `pub_from_priv(public_key, private_key)` to instead treat the payload as private key
	    valid_key_counter++; // for next round
    } while (!is_valid_pubkey(public_key));
    ESP_LOGI(LOG_TAG, "  pub key to use (%d. try): %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ... %02x", valid_key_counter, public_key[0], public_key[1], public_key[2], public_key[3], public_key[4], public_key[5], public_key[6], public_key[7], public_key[8], public_key[9], public_key[10], public_key[11], public_key[12], public_key[13],public_key[14], public_key[15],public_key[16],public_key[17],public_key[19], public_key[19], public_key[20], public_key[21], public_key[22], public_key[23], public_key[24], public_key[25], public_key[26],  public_key[27]);
    set_addr_from_key(rnd_addr, public_key);
    set_payload_from_key(adv_data, public_key);
}

// No error handling yet
void reset_advertising() {
    esp_err_t status;
    esp_ble_gap_stop_advertising();
    if ((status = esp_ble_gap_set_rand_addr(rnd_addr)) != ESP_OK) {
        ESP_LOGE(LOG_TAG, "couldn't set random address: %s", esp_err_to_name(status));
        return;
    }
    if ((esp_ble_gap_config_adv_data_raw((uint8_t*)&adv_data, sizeof(adv_data))) != ESP_OK) {
        ESP_LOGE(LOG_TAG, "couldn't configure BLE adv: %s", esp_err_to_name(status));
        return;
    }
}

void send_data_once_blocking(uint8_t* data_to_send, uint32_t len, uint32_t msg_id) {
    ESP_LOGI(LOG_TAG, "Data to send (msg_id: %d): %s", msg_id, data_to_send);

    uint8_t current_bit = 0;
    // iterate byte-by-byte
    for(int by_i = 0; by_i < len; by_i++) {
        ESP_LOGI(LOG_TAG, "  Sending byte %d/%d (0x%02x)", by_i, len-1, data_to_send[by_i]);
        // iterate bit-by-bit
        for(int bi_i = 0; bi_i < 8; bi_i++) {
            if (CHECK_BIT(data_to_send[by_i], bi_i)) {
                current_bit = 1;
            }
            else {
                current_bit = 0;
            }
            ESP_LOGD(LOG_TAG, "  Sending byte %d, bit %d: %d", by_i, bi_i, current_bit);
            set_addr_and_payload_for_bit(by_i*8+bi_i, msg_id, current_bit);
            ESP_LOGD(LOG_TAG, "    resetting. Will now use device address: %02x %02x %02x %02x %02x %02x", rnd_addr[0], rnd_addr[1], rnd_addr[2], rnd_addr[3], rnd_addr[4], rnd_addr[5]);
            reset_advertising();
            vTaskDelay(2);
        }
    }
    esp_ble_gap_stop_advertising();
}

uint8_t* read_line_or_dismiss(int* len) {
    uint8_t *line = (uint8_t *) malloc(BUF_SIZE);
    int size;
    uint8_t *ptr = line;
    while(1) {
        size = uart_read_bytes(UART_PORT_NUM, (unsigned char *)ptr, 1, 20 / portTICK_RATE_MS);
        if (size == 1) {
            if (*ptr == '\n') {
                *ptr = 0;
                *len = ptr-line;
                return line;
            }
            ptr++;
        }
        else { free(line); ESP_LOGI(LOG_TAG, "Dismissing line"); return 0; }
    }
}

void init_serial() {
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    int intr_alloc_flags = 0;

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, TEST_RTS, TEST_CTS));
}


void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);

    esp_bluedroid_init();
    esp_bluedroid_enable();

    // Initial test message sent after boot
    static uint8_t data_to_send[] = "TEST MESSAGE";

    esp_err_t status;
    //register the scan callback function to the gap module
    if ((status = esp_ble_gap_register_callback(esp_gap_cb)) != ESP_OK) {
        ESP_LOGE(LOG_TAG, "gap register error: %s", esp_err_to_name(status));
        return;
    }

    uint32_t current_message_id = 0;
    
    ESP_LOGI(LOG_TAG, "Sending initial default message: %s", data_to_send);

    send_data_once_blocking(data_to_send, sizeof(data_to_send), current_message_id);

    ESP_LOGI(LOG_TAG, "Entering serial modem mode");
    init_serial();

    // UART test line
    uart_write_bytes(UART_PORT_NUM, (const char *) "Serial activated. Waiting for text lines.\n", 42);

    int len = sizeof(data_to_send);
    uint8_t *data = data_to_send; // allocated by serial reader
    uint8_t *new_data = 0;

    while (1) {
        if((new_data = read_line_or_dismiss(&len))) {
            data = new_data;
            current_message_id++;
            ESP_LOGI(LOG_TAG, "Received line (len: %d): %s", len, data);
        }
        else {
            ESP_LOGI(LOG_TAG, "No new input. Continuing sending old data");
        }
        if(data) { // should always be set
            send_data_once_blocking(data, len, current_message_id);
        }
        vTaskDelay(200);
    }
    esp_ble_gap_stop_advertising();
}
