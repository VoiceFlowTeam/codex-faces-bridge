#include "codex_hid.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "esp_hidd_transport.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define CODEX_HID_REPORT_ID 6
#define CODEX_HID_REPORT_DATA_SIZE 63
#define CODEX_HID_RPC_CHANNEL 2
#define CODEX_HID_RPC_CHUNK_SIZE 61
#define CODEX_HID_MTU 185
#define CODEX_ADV_DATA_PENDING 0x01
#define CODEX_SCAN_RESPONSE_PENDING 0x02

#define CODEX_VID 0x303A
#define CODEX_PID 0x8360

static const char *TAG = "codex_hid";

typedef struct {
    uint8_t length;
    uint8_t data[CODEX_HID_REPORT_DATA_SIZE];
} rx_packet_t;

static esp_hidd_dev_t *s_hid_device;
static codex_hid_rx_fn s_rx_fn;
static void *s_rx_context;
static codex_hid_connection_fn s_connection_fn;
static void *s_connection_context;
static QueueHandle_t s_rx_queue;
static SemaphoreHandle_t s_tx_mutex;
static bool s_connected;
static bool s_hid_ready;
static bool s_advertising;
static uint8_t s_adv_config_pending;

// Vendor-defined application collection. Report ID 6 carries 63 data bytes;
// node-hid prepends the report ID and therefore exposes the expected 64 bytes.
static const uint8_t s_codex_report_map[] = {
    0x06, 0x00, 0xFF,       // Usage Page (Vendor 0xFF00)
    0x09, 0x01,             // Usage (1)
    0xA1, 0x01,             // Collection (Application)
    0x85, CODEX_HID_REPORT_ID,
    0x15, 0x00,             // Logical minimum 0
    0x26, 0xFF, 0x00,       // Logical maximum 255
    0x75, 0x08,             // 8 bits per field
    0x95, CODEX_HID_REPORT_DATA_SIZE,
    0x09, 0x01,
    0x81, 0x02,             // Input (Data, Variable, Absolute)
    0x95, CODEX_HID_REPORT_DATA_SIZE,
    0x09, 0x01,
    0x91, 0x02,             // Output (Data, Variable, Absolute)
    0xC0,
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {
        .data = s_codex_report_map,
        .len = sizeof(s_codex_report_map),
    },
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id = CODEX_VID,
    .product_id = CODEX_PID,
    .version = 0x0100,
    .device_name = "kbd-1.0-codex-micro",
    .manufacturer_name = "Work Louder",
    .serial_number = "FIRE-FACES-0001",
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

static const uint8_t s_hid_service_uuid128[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = false,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = ESP_HID_APPEARANCE_GENERIC,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(s_hid_service_uuid128),
    .p_service_uuid = (uint8_t *)s_hid_service_uuid128,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static esp_ble_adv_data_t s_scan_response_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = true,
    .appearance = ESP_HID_APPEARANCE_GENERIC,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void notify_connection(bool connected)
{
    if (s_connected == connected) {
        return;
    }
    s_connected = connected;
    if (s_connection_fn != NULL) {
        s_connection_fn(connected, s_connection_context);
    }
}

static void try_start_advertising(void)
{
    if (!s_connected && !s_advertising && s_hid_ready && s_adv_config_pending == 0) {
        const esp_err_t status = esp_ble_gap_start_advertising(&s_adv_params);
        if (status == ESP_OK) {
            s_advertising = true;
        } else {
            ESP_LOGW(TAG, "Failed to start advertising: %s", esp_err_to_name(status));
        }
    }
}

static void rx_task(void *argument)
{
    rx_packet_t packet;
    while (true) {
        if (xQueueReceive(s_rx_queue, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_rx_fn == NULL || packet.length < 2) {
            continue;
        }
        if (packet.data[0] != CODEX_HID_RPC_CHANNEL) {
            ESP_LOGD(TAG, "Ignoring HID channel %u", packet.data[0]);
            continue;
        }
        size_t payload_length = packet.data[1];
        if (payload_length > CODEX_HID_RPC_CHUNK_SIZE) {
            payload_length = CODEX_HID_RPC_CHUNK_SIZE;
        }
        if (payload_length + 2 > packet.length) {
            payload_length = packet.length - 2;
        }
        s_rx_fn(packet.data + 2, payload_length, s_rx_context);
    }
}

static void queue_output_report(const esp_hidd_event_data_t *param)
{
    if (param->output.report_id != CODEX_HID_REPORT_ID || param->output.data == NULL) {
        return;
    }

    const uint8_t *source = param->output.data;
    size_t length = param->output.length;
    // Some HID backends include the report ID in the callback payload and
    // others expose it separately. Normalize both forms.
    if (length > 0 && source[0] == CODEX_HID_REPORT_ID) {
        source += 1;
        length -= 1;
    }
    if (length > CODEX_HID_REPORT_DATA_SIZE) {
        length = CODEX_HID_REPORT_DATA_SIZE;
    }

    rx_packet_t packet = {
        .length = (uint8_t)length,
    };
    memcpy(packet.data, source, length);
    if (xQueueSend(s_rx_queue, &packet, 0) != pdTRUE) {
        ESP_LOGW(TAG, "RPC receive queue full; dropping HID report");
    }
}

static void hidd_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidd_event_data_t *param = event_data;

    switch ((esp_hidd_event_t)event_id) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID service ready");
        s_hid_ready = true;
        try_start_advertising();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "Codex host connected");
        s_advertising = false;
        notify_connection(true);
        break;
    case ESP_HIDD_OUTPUT_EVENT:
        queue_output_report(param);
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "Codex host disconnected (reason=%d)", param->disconnect.reason);
        notify_connection(false);
        s_advertising = false;
        try_start_advertising();
        break;
    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG, "HID protocol mode: %u", param->protocol_mode.protocol_mode);
        break;
    default:
        break;
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_adv_config_pending &= ~CODEX_ADV_DATA_PENDING;
        try_start_advertising();
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        s_adv_config_pending &= ~CODEX_SCAN_RESPONSE_PENDING;
        try_start_advertising();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            s_advertising = false;
            ESP_LOGW(TAG, "Advertising start failed: %d", param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "Advertising as %s", s_hid_config.device_name);
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_advertising = false;
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "BLE pairing completed");
        } else {
            ESP_LOGW(TAG, "BLE pairing failed: 0x%x", param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG, "BLE interval=%u latency=%u mtu=%u",
                 param->update_conn_params.conn_int,
                 param->update_conn_params.latency,
                 CODEX_HID_MTU);
        break;
    default:
        break;
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t status = nvs_flash_init();
    if (status == ESP_ERR_NVS_NO_FREE_PAGES || status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS");
        status = nvs_flash_init();
    }
    return status;
}

static esp_err_t init_bluetooth(void)
{
    ESP_RETURN_ON_ERROR(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT), TAG, "release classic BT memory");

    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if CONFIG_IDF_TARGET_ESP32
    controller_config.mode = ESP_BT_MODE_BLE;
#endif
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&controller_config), TAG, "init BT controller");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), TAG, "enable BLE controller");

    esp_bluedroid_config_t bluedroid_config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bluedroid_init_with_cfg(&bluedroid_config), TAG, "init Bluedroid");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "enable Bluedroid");
    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_event_handler), TAG, "register GAP callback");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler), TAG, "register GATTS callback");
    ESP_RETURN_ON_ERROR(esp_ble_gatt_set_local_mtu(CODEX_HID_MTU), TAG, "set local MTU");
    return ESP_OK;
}

static esp_err_t configure_advertising_and_security(void)
{
    esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t io_capability = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t response_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth)), TAG, "set BLE auth");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_capability, sizeof(io_capability)), TAG, "set BLE IO capability");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size)), TAG, "set BLE key size");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key)), TAG, "set BLE initiator keys");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &response_key, sizeof(response_key)), TAG, "set BLE responder keys");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_device_name(s_hid_config.device_name), TAG, "set BLE name");
    s_adv_config_pending = CODEX_ADV_DATA_PENDING | CODEX_SCAN_RESPONSE_PENDING;
    ESP_RETURN_ON_ERROR(esp_ble_gap_config_adv_data(&s_adv_data), TAG, "configure advertising");
    ESP_RETURN_ON_ERROR(esp_ble_gap_config_adv_data(&s_scan_response_data), TAG, "configure scan response");
    return ESP_OK;
}

esp_err_t codex_hid_init(codex_hid_rx_fn rx_fn,
                         void *rx_context,
                         codex_hid_connection_fn connection_fn,
                         void *connection_context)
{
    if (rx_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_rx_fn = rx_fn;
    s_rx_context = rx_context;
    s_connection_fn = connection_fn;
    s_connection_context = connection_context;
    s_rx_queue = xQueueCreate(24, sizeof(rx_packet_t));
    s_tx_mutex = xSemaphoreCreateMutex();
    if (s_rx_queue == NULL || s_tx_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(rx_task, "codex_rpc_rx", 6144, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "init NVS");
    ESP_RETURN_ON_ERROR(init_bluetooth(), TAG, "init Bluetooth");
    ESP_RETURN_ON_ERROR(configure_advertising_and_security(), TAG, "configure BLE HID");
    ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&s_hid_config,
                                          ESP_HID_TRANSPORT_BLE,
                                          hidd_event_handler,
                                          &s_hid_device),
                        TAG,
                        "init HID device");
    return ESP_OK;
}

esp_err_t codex_hid_send_text(const char *text, size_t length, void *context)
{
    (void)context;
    if (text == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_connected || s_hid_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t status = ESP_OK;
    size_t offset = 0;
    while (offset < length) {
        uint8_t report[CODEX_HID_REPORT_DATA_SIZE] = {0};
        size_t chunk = length - offset;
        if (chunk > CODEX_HID_RPC_CHUNK_SIZE) {
            chunk = CODEX_HID_RPC_CHUNK_SIZE;
        }
        report[0] = CODEX_HID_RPC_CHANNEL;
        report[1] = (uint8_t)chunk;
        memcpy(report + 2, text + offset, chunk);
        status = esp_hidd_dev_input_set(s_hid_device,
                                        0,
                                        CODEX_HID_REPORT_ID,
                                        report,
                                        sizeof(report));
        if (status != ESP_OK) {
            break;
        }
        offset += chunk;
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    xSemaphoreGive(s_tx_mutex);
    return status;
}

bool codex_hid_is_connected(void)
{
    return s_connected;
}
