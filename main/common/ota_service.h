#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define OTA_SERVICE_URL_MAX_LEN       256
#define OTA_SERVICE_VERSION_MAX_LEN   32
#define OTA_SERVICE_SHA256_HEX_LEN    65
#define OTA_SERVICE_ERROR_MAX_LEN     96

typedef enum {
    OTA_SERVICE_STATE_IDLE = 0,
    OTA_SERVICE_STATE_FETCH_MANIFEST,
    OTA_SERVICE_STATE_CHECK_MANIFEST,
    OTA_SERVICE_STATE_DOWNLOADING,
    OTA_SERVICE_STATE_VERIFYING,
    OTA_SERVICE_STATE_READY_REBOOT,
    OTA_SERVICE_STATE_FAILED,
    OTA_SERVICE_STATE_CANCELED,
} ota_service_state_t;

typedef struct {
    ota_service_state_t state;
    int progress;
    bool busy;
    bool resume_valid;
    char version[OTA_SERVICE_VERSION_MAX_LEN];
    char error[OTA_SERVICE_ERROR_MAX_LEN];
    size_t written_size;
    size_t total_size;
} ota_service_snapshot_t;

esp_err_t ota_service_init(void);
void ota_service_mark_app_valid_if_needed(void);
void ota_service_notify_wifi_state(void);

esp_err_t ota_service_start(const char *manifest_url);
esp_err_t ota_service_resume(void);
esp_err_t ota_service_cancel(void);
esp_err_t ota_service_reboot_if_ready(void);

void ota_service_get_snapshot(ota_service_snapshot_t *out);
const char *ota_service_state_to_string(ota_service_state_t state);
int ota_service_format_status_json(char *out, size_t out_size);
int ota_service_format_status_object_json(char *out, size_t out_size);
