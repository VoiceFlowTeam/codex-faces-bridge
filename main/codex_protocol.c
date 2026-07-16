#include "codex_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CODEX_RPC_BUFFER_SIZE 2048
#define CODEX_FIRMWARE_VERSION "0.2.0-fire-faces"

static const char *TAG = "codex_rpc";

static codex_protocol_send_fn s_send_fn;
static void *s_send_context;
static codex_protocol_lighting_fn s_lighting_fn;
static void *s_lighting_context;
static codex_protocol_power_fn s_power_fn;
static void *s_power_context;
static codex_lighting_state_t s_lighting;
static char s_rpc_buffer[CODEX_RPC_BUFFER_SIZE];
static size_t s_rpc_length;
static SemaphoreHandle_t s_send_mutex;
static int s_profile_index;
static int s_layer_index;

static esp_err_t send_json(cJSON *message)
{
    char *encoded = cJSON_PrintUnformatted(message);
    if (encoded == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const size_t encoded_length = strlen(encoded);
    char *line = malloc(encoded_length + 2);
    if (line == NULL) {
        cJSON_free(encoded);
        return ESP_ERR_NO_MEM;
    }
    memcpy(line, encoded, encoded_length);
    line[encoded_length] = '\n';
    line[encoded_length + 1] = '\0';

    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_send_fn != NULL && xSemaphoreTake(s_send_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        result = s_send_fn(line, encoded_length + 1, s_send_context);
        xSemaphoreGive(s_send_mutex);
    }

    free(line);
    cJSON_free(encoded);
    return result;
}

static cJSON *duplicate_request_id(const cJSON *request)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "id");
    if (id == NULL) {
        id = cJSON_GetObjectItemCaseSensitive(request, "i");
    }
    return id == NULL ? cJSON_CreateNull() : cJSON_Duplicate(id, true);
}

static esp_err_t send_result(const cJSON *request, cJSON *result)
{
    cJSON *response = cJSON_CreateObject();
    if (response == NULL || result == NULL) {
        cJSON_Delete(response);
        cJSON_Delete(result);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(response, "id", duplicate_request_id(request));
    cJSON_AddItemToObject(response, "result", result);
    const esp_err_t status = send_json(response);
    cJSON_Delete(response);
    return status;
}

static esp_err_t send_error(const cJSON *request, int code, const char *message)
{
    cJSON *response = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    if (response == NULL || error == NULL) {
        cJSON_Delete(response);
        cJSON_Delete(error);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(response, "id", duplicate_request_id(request));
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(response, "error", error);
    const esp_err_t status = send_json(response);
    cJSON_Delete(response);
    return status;
}

static double number_or(const cJSON *object, const char *name, double fallback)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(value) ? value->valuedouble : fallback;
}

static void apply_thread_lighting(const cJSON *params)
{
    if (!cJSON_IsArray(params)) {
        return;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, params) {
        const cJSON *id_json = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (!cJSON_IsNumber(id_json)) {
            continue;
        }
        const int id = id_json->valueint;
        if (id < 0 || id >= CODEX_THREAD_COUNT) {
            continue;
        }

        codex_thread_lighting_t *thread = &s_lighting.threads[id];
        thread->id = (uint8_t)id;
        thread->color = (uint32_t)number_or(item, "c", thread->color);
        thread->brightness = (float)number_or(item, "b", thread->brightness);
        thread->effect = (uint8_t)number_or(item, "e", thread->effect);
        thread->speed = (float)number_or(item, "s", thread->speed);
        thread->sync_keys_lighting = number_or(item, "sk", thread->sync_keys_lighting ? 1 : 0) != 0;
        thread->sync_ambient_lighting = number_or(item, "sa", thread->sync_ambient_lighting ? 1 : 0) != 0;
    }

    if (s_lighting_fn != NULL) {
        s_lighting_fn(&s_lighting, s_lighting_context);
    }
}

static void apply_lighting_side(const cJSON *params, codex_lighting_side_t *side)
{
    if (!cJSON_IsObject(params) || side == NULL) {
        return;
    }
    side->color = (uint32_t)number_or(params, "c", side->color);
    side->brightness = (float)number_or(params, "b", side->brightness);
    side->effect = (uint8_t)number_or(params, "e", side->effect);
    side->speed = (float)number_or(params, "s", side->speed);
    side->magic = (float)number_or(params, "m", side->magic);
}

static void apply_rgb_config(const cJSON *params)
{
    if (!cJSON_IsObject(params)) {
        return;
    }
    const cJSON *ambient = cJSON_GetObjectItemCaseSensitive(params, "ambient");
    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(params, "keys");
    apply_lighting_side(ambient, &s_lighting.ambient);
    apply_lighting_side(keys, &s_lighting.keys);
    if (s_lighting_fn != NULL) {
        s_lighting_fn(&s_lighting, s_lighting_context);
    }
}

static void process_request(const char *line)
{
    cJSON *request = cJSON_Parse(line);
    if (request == NULL) {
        ESP_LOGW(TAG, "Ignoring malformed JSON-RPC message");
        return;
    }

    const cJSON *method_json = cJSON_GetObjectItemCaseSensitive(request, "method");
    if (!cJSON_IsString(method_json)) {
        method_json = cJSON_GetObjectItemCaseSensitive(request, "m");
    }
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(request, "params");
    if (params == NULL) {
        params = cJSON_GetObjectItemCaseSensitive(request, "p");
    }

    if (!cJSON_IsString(method_json)) {
        send_error(request, -32600, "Invalid request");
        cJSON_Delete(request);
        return;
    }

    const char *method = method_json->valuestring;
    ESP_LOGD(TAG, "RPC %s", method);
    if (strcmp(method, "sys.version") == 0) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "version", CODEX_FIRMWARE_VERSION);
        send_result(request, result);
    } else if (strcmp(method, "device.status") == 0) {
        codex_power_status_t power = {
            .battery_percent = 0,
            .is_charging = false,
            .available = false,
        };
        if (s_power_fn != NULL) {
            s_power_fn(&power, s_power_context);
        }
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "version", CODEX_FIRMWARE_VERSION);
        cJSON_AddNumberToObject(result, "profile_index", s_profile_index);
        cJSON_AddNumberToObject(result, "layer_index", s_layer_index);
        cJSON_AddNumberToObject(result, "battery", power.battery_percent);
        cJSON_AddBoolToObject(result, "is_charging", power.is_charging);
        send_result(request, result);
    } else if (strcmp(method, "v.oai.thstatus") == 0) {
        apply_thread_lighting(params);
        send_result(request, cJSON_CreateBool(true));
    } else if (strcmp(method, "v.oai.rgbcfg") == 0) {
        apply_rgb_config(params);
        send_result(request, cJSON_CreateBool(true));
    } else {
        ESP_LOGW(TAG, "Unsupported RPC method: %s", method);
        send_error(request, -32601, "Method not found");
    }

    cJSON_Delete(request);
}

esp_err_t codex_protocol_init(codex_protocol_send_fn send_fn,
                              void *send_context,
                              codex_protocol_lighting_fn lighting_fn,
                              void *lighting_context,
                              codex_protocol_power_fn power_fn,
                              void *power_context)
{
    if (send_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_send_fn = send_fn;
    s_send_context = send_context;
    s_lighting_fn = lighting_fn;
    s_lighting_context = lighting_context;
    s_power_fn = power_fn;
    s_power_context = power_context;
    s_rpc_length = 0;
    s_profile_index = 0;
    s_layer_index = 0;
    memset(&s_lighting, 0, sizeof(s_lighting));
    for (int i = 0; i < CODEX_THREAD_COUNT; ++i) {
        s_lighting.threads[i].id = (uint8_t)i;
    }
    s_send_mutex = xSemaphoreCreateMutex();
    return s_send_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

void codex_protocol_feed(const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0) {
        return;
    }

    for (size_t i = 0; i < length; ++i) {
        const char byte = (char)data[i];
        if (byte == '\n') {
            s_rpc_buffer[s_rpc_length] = '\0';
            if (s_rpc_length > 0) {
                process_request(s_rpc_buffer);
            }
            s_rpc_length = 0;
            continue;
        }
        if (byte == '\r' || byte == '\0') {
            continue;
        }
        if (s_rpc_length + 1 >= sizeof(s_rpc_buffer)) {
            ESP_LOGW(TAG, "RPC receive buffer overflow; dropping message");
            s_rpc_length = 0;
            continue;
        }
        s_rpc_buffer[s_rpc_length++] = byte;
    }
}

void codex_protocol_set_device_state(int profile_index, int layer_index)
{
    s_profile_index = profile_index;
    s_layer_index = layer_index;
}

esp_err_t codex_protocol_send_hid_action(const char *key, int action, int agent_index)
{
    if (key == NULL || action < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *message = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    if (message == NULL || params == NULL) {
        cJSON_Delete(message);
        cJSON_Delete(params);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(message, "method", "v.oai.hid");
    cJSON_AddStringToObject(params, "k", key);
    cJSON_AddNumberToObject(params, "act", action);
    if (agent_index >= 0) {
        cJSON_AddNumberToObject(params, "ag", agent_index);
    }
    cJSON_AddItemToObject(message, "params", params);
    const esp_err_t status = send_json(message);
    cJSON_Delete(message);
    return status;
}

esp_err_t codex_protocol_send_key(const char *key, bool pressed, int agent_index)
{
    return codex_protocol_send_hid_action(key, pressed ? 1 : 0, agent_index);
}

esp_err_t codex_protocol_send_joystick(float angle, float distance)
{
    cJSON *message = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    if (message == NULL || params == NULL) {
        cJSON_Delete(message);
        cJSON_Delete(params);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(message, "method", "v.oai.rad");
    cJSON_AddNumberToObject(params, "a", angle);
    cJSON_AddNumberToObject(params, "d", distance);
    cJSON_AddItemToObject(message, "params", params);
    const esp_err_t status = send_json(message);
    cJSON_Delete(message);
    return status;
}
