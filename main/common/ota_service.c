#include "ota_service.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>

#include "app_log.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "wifi_manager.h"

/*
 * OTA 服务采用 manifest 驱动，固件下载固定走 HTTPS。
 * 为了支持断点续传，下载过程不使用 esp_https_ota 的高层封装，而是：
 * 1. esp_http_client 手动设置 Range；
 * 2. esp_ota_begin / esp_ota_resume 管理 OTA 写入；
 * 3. mbedtls_sha256 对目标分区已写入内容做整包校验。
 */

esp_err_t esp_crt_bundle_attach(void *conf);

static const char *TAG = "ota_service";

#define OTA_NVS_NS                  "ota_state"
#define OTA_MANIFEST_MAX_LEN        1024
#define OTA_HTTP_BUF_SIZE           4096
#define OTA_SAVE_STEP_BYTES         (64 * 1024)
#define OTA_MAX_FAIL_COUNT          3
#define OTA_TASK_STACK_SIZE         8192
#define OTA_TASK_PRIORITY           5
#define OTA_HTTP_TIMEOUT_MS         15000

typedef struct {
    bool valid;
    char manifest_url[OTA_SERVICE_URL_MAX_LEN];
    char firmware_url[OTA_SERVICE_URL_MAX_LEN];
    char version[OTA_SERVICE_VERSION_MAX_LEN];
    char sha256[OTA_SERVICE_SHA256_HEX_LEN];
    size_t size;
    size_t written_size;
    int partition_subtype;
    int fail_count;
} ota_resume_record_t;

typedef struct {
    char project[32];
    char chip[16];
    char version[OTA_SERVICE_VERSION_MAX_LEN];
    char url[OTA_SERVICE_URL_MAX_LEN];
    char sha256[OTA_SERVICE_SHA256_HEX_LEN];
    size_t size;
    bool force;
} ota_manifest_t;

static SemaphoreHandle_t s_lock;
static ota_service_snapshot_t s_snapshot;
static ota_resume_record_t s_resume;
static TaskHandle_t s_task;
static bool s_cancel_requested;
static char s_pending_manifest_url[OTA_SERVICE_URL_MAX_LEN];
static uint8_t s_http_buf[OTA_HTTP_BUF_SIZE];

static void lock_state(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock_state(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static bool starts_with_https(const char *url)
{
    return url != NULL && strncmp(url, "https://", 8) == 0;
}

static bool is_sha256_hex(const char *s)
{
    if (s == NULL || strlen(s) != 64) {
        return false;
    }

    for (int i = 0; i < 64; ++i) {
        if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

static void set_state(ota_service_state_t state, const char *error)
{
    lock_state();
    s_snapshot.state = state;
    s_snapshot.busy = (state == OTA_SERVICE_STATE_FETCH_MANIFEST ||
                       state == OTA_SERVICE_STATE_CHECK_MANIFEST ||
                       state == OTA_SERVICE_STATE_DOWNLOADING ||
                       state == OTA_SERVICE_STATE_VERIFYING);
    if (error != NULL) {
        strlcpy(s_snapshot.error, error, sizeof(s_snapshot.error));
    } else if (state != OTA_SERVICE_STATE_FAILED) {
        s_snapshot.error[0] = '\0';
    }
    unlock_state();
}

static void update_progress(size_t written, size_t total)
{
    lock_state();
    s_snapshot.written_size = written;
    s_snapshot.total_size = total;
    s_snapshot.progress = (total > 0) ? (int)((written * 100) / total) : 0;
    if (s_snapshot.progress > 100) {
        s_snapshot.progress = 100;
    }
    unlock_state();
}

static void set_resume_valid(bool valid)
{
    lock_state();
    s_snapshot.resume_valid = valid;
    unlock_state();
}

static esp_err_t open_nvs(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(OTA_NVS_NS, mode, handle);
}

static esp_err_t load_resume_record(void)
{
    memset(&s_resume, 0, sizeof(s_resume));

    nvs_handle_t handle;
    esp_err_t err = open_nvs(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        set_resume_valid(false);
        return err;
    }

    uint8_t valid = 0;
    nvs_get_u8(handle, "valid", &valid);
    s_resume.valid = (valid != 0);

    size_t len = sizeof(s_resume.manifest_url);
    nvs_get_str(handle, "manifest", s_resume.manifest_url, &len);
    len = sizeof(s_resume.firmware_url);
    nvs_get_str(handle, "fw_url", s_resume.firmware_url, &len);
    len = sizeof(s_resume.version);
    nvs_get_str(handle, "version", s_resume.version, &len);
    len = sizeof(s_resume.sha256);
    nvs_get_str(handle, "sha256", s_resume.sha256, &len);

    uint32_t size = 0;
    uint32_t written = 0;
    int32_t subtype = -1;
    int32_t fail_count = 0;
    nvs_get_u32(handle, "size", &size);
    nvs_get_u32(handle, "written", &written);
    nvs_get_i32(handle, "subtype", &subtype);
    nvs_get_i32(handle, "fail_count", &fail_count);
    nvs_close(handle);

    s_resume.size = size;
    s_resume.written_size = written;
    s_resume.partition_subtype = subtype;
    s_resume.fail_count = fail_count;

    bool usable = s_resume.valid &&
                  s_resume.manifest_url[0] != '\0' &&
                  s_resume.firmware_url[0] != '\0' &&
                  s_resume.size > 0 &&
                  s_resume.written_size > 0 &&
                  s_resume.written_size < s_resume.size &&
                  s_resume.partition_subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
                  s_resume.partition_subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX &&
                  is_sha256_hex(s_resume.sha256);
    set_resume_valid(usable);
    if (usable) {
        lock_state();
        strlcpy(s_snapshot.version, s_resume.version, sizeof(s_snapshot.version));
        s_snapshot.total_size = s_resume.size;
        s_snapshot.written_size = s_resume.written_size;
        s_snapshot.progress = (int)((s_resume.written_size * 100) / s_resume.size);
        unlock_state();
    }
    return ESP_OK;
}

static esp_err_t save_resume_record(void)
{
    nvs_handle_t handle;
    esp_err_t err = open_nvs(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_set_u8(handle, "valid", s_resume.valid ? 1 : 0);
    nvs_set_str(handle, "manifest", s_resume.manifest_url);
    nvs_set_str(handle, "fw_url", s_resume.firmware_url);
    nvs_set_str(handle, "version", s_resume.version);
    nvs_set_str(handle, "sha256", s_resume.sha256);
    nvs_set_u32(handle, "size", (uint32_t)s_resume.size);
    nvs_set_u32(handle, "written", (uint32_t)s_resume.written_size);
    nvs_set_i32(handle, "subtype", s_resume.partition_subtype);
    nvs_set_i32(handle, "fail_count", s_resume.fail_count);
    err = nvs_commit(handle);
    nvs_close(handle);
    set_resume_valid(s_resume.valid);
    return err;
}

static void clear_resume_record(void)
{
    nvs_handle_t handle;
    if (open_nvs(NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    memset(&s_resume, 0, sizeof(s_resume));
    set_resume_valid(false);
}

static const esp_partition_t *find_partition_by_subtype(int subtype)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    (esp_partition_subtype_t)subtype,
                                    NULL);
}

static esp_err_t http_get_to_buffer(const char *url, char *out, size_t out_size)
{
    if (!starts_with_https(url) || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "http open failed url=%s", url);
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || content_len < 0 || (size_t)content_len >= out_size) {
        APP_LOGE(TAG, "http buffer request invalid status=%d content_len=%lld url=%s",
                 status, (long long)content_len, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int read_total = 0;
    while (read_total < content_len) {
        int r = esp_http_client_read(client, out + read_total, (int)(out_size - 1 - read_total));
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        read_total += r;
    }

    out[read_total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static bool json_get_string(cJSON *obj, const char *key, char *out, size_t out_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    strlcpy(out, item->valuestring, out_size);
    return true;
}

static esp_err_t parse_manifest(const char *json, ota_manifest_t *manifest)
{
    if (json == NULL || manifest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(manifest, 0, sizeof(*manifest));
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    bool ok = json_get_string(root, "project", manifest->project, sizeof(manifest->project)) &&
              json_get_string(root, "chip", manifest->chip, sizeof(manifest->chip)) &&
              json_get_string(root, "version", manifest->version, sizeof(manifest->version)) &&
              json_get_string(root, "url", manifest->url, sizeof(manifest->url)) &&
              json_get_string(root, "sha256", manifest->sha256, sizeof(manifest->sha256));

    cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!cJSON_IsNumber(size) || size->valuedouble <= 0) {
        ok = false;
    } else {
        manifest->size = (size_t)size->valuedouble;
    }

    cJSON *force = cJSON_GetObjectItemCaseSensitive(root, "force");
    manifest->force = cJSON_IsTrue(force);

    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t validate_manifest(const ota_manifest_t *manifest, const esp_partition_t *target)
{
    if (manifest == NULL || target == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    if (strcmp(manifest->project, running->project_name) != 0) {
        APP_LOGE(TAG, "manifest project mismatch manifest=%s app=%s",
                 manifest->project, running->project_name);
        return ESP_ERR_INVALID_VERSION;
    }

    if (strcmp(manifest->chip, "esp32s3") != 0) {
        APP_LOGE(TAG, "manifest chip mismatch chip=%s", manifest->chip);
        return ESP_ERR_INVALID_VERSION;
    }

    if (!manifest->force && strcmp(manifest->version, running->version) == 0) {
        APP_LOGW(TAG, "manifest version equals running version=%s", manifest->version);
        return ESP_ERR_INVALID_VERSION;
    }

    if (!starts_with_https(manifest->url) || !is_sha256_hex(manifest->sha256)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (manifest->size == 0 || manifest->size > target->size) {
        APP_LOGE(TAG, "manifest size invalid size=%u partition=%u",
                 (unsigned)manifest->size, (unsigned)target->size);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static bool manifest_matches_resume(const char *manifest_url, const ota_manifest_t *manifest)
{
    return s_resume.valid &&
           strcmp(s_resume.manifest_url, manifest_url) == 0 &&
           strcmp(s_resume.firmware_url, manifest->url) == 0 &&
           strcmp(s_resume.version, manifest->version) == 0 &&
           strcmp(s_resume.sha256, manifest->sha256) == 0 &&
           s_resume.size == manifest->size;
}

static void manifest_to_resume(const char *manifest_url, const ota_manifest_t *manifest, const esp_partition_t *target)
{
    memset(&s_resume, 0, sizeof(s_resume));
    s_resume.valid = true;
    strlcpy(s_resume.manifest_url, manifest_url, sizeof(s_resume.manifest_url));
    strlcpy(s_resume.firmware_url, manifest->url, sizeof(s_resume.firmware_url));
    strlcpy(s_resume.version, manifest->version, sizeof(s_resume.version));
    strlcpy(s_resume.sha256, manifest->sha256, sizeof(s_resume.sha256));
    s_resume.size = manifest->size;
    s_resume.partition_subtype = target->subtype;
}

static void hex_lower(const uint8_t *in, size_t in_len, char *out, size_t out_size)
{
    static const char hexdig[] = "0123456789abcdef";
    if (out_size < in_len * 2 + 1) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return;
    }

    for (size_t i = 0; i < in_len; ++i) {
        out[i * 2] = hexdig[in[i] >> 4];
        out[i * 2 + 1] = hexdig[in[i] & 0x0f];
    }
    out[in_len * 2] = '\0';
}

static esp_err_t sha256_partition(const esp_partition_t *partition, size_t size, char out_hex[OTA_SERVICE_SHA256_HEX_LEN])
{
    mbedtls_sha256_context ctx;
    uint8_t digest[32];
    size_t offset = 0;
    esp_err_t ret = ESP_OK;

    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }

    while (offset < size) {
        size_t to_read = size - offset;
        if (to_read > OTA_HTTP_BUF_SIZE) {
            to_read = OTA_HTTP_BUF_SIZE;
        }
        ret = esp_partition_read(partition, offset, s_http_buf, to_read);
        if (ret != ESP_OK) {
            break;
        }
        if (mbedtls_sha256_update(&ctx, s_http_buf, to_read) != 0) {
            ret = ESP_FAIL;
            break;
        }
        offset += to_read;
    }

    if (ret == ESP_OK && mbedtls_sha256_finish(&ctx, digest) != 0) {
        ret = ESP_FAIL;
    }
    mbedtls_sha256_free(&ctx);

    if (ret == ESP_OK) {
        hex_lower(digest, sizeof(digest), out_hex, OTA_SERVICE_SHA256_HEX_LEN);
    }
    return ret;
}

static esp_err_t download_firmware(const char *manifest_url, const ota_manifest_t *manifest, const esp_partition_t *target)
{
    bool resume = s_resume.valid && s_resume.written_size > 0;
retry_from_start:
    ;
    size_t written = resume ? s_resume.written_size : 0;
    size_t last_saved = written;
    esp_ota_handle_t ota_handle = 0;

    esp_http_client_config_t cfg = {
        .url = manifest->url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = OTA_HTTP_BUF_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char range_header[48];
    if (resume) {
        snprintf(range_header, sizeof(range_header), "bytes=%u-", (unsigned)written);
        esp_http_client_set_header(client, "Range", range_header);
        APP_LOGI(TAG, "resume OTA from offset=%u url=%s", (unsigned)written, manifest->url);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "firmware http open failed");
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if ((!resume && status != 200) || (resume && status != 206)) {
        APP_LOGE(TAG, "unexpected firmware http status=%d resume=%d", status, resume ? 1 : 0);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (resume) {
            clear_resume_record();
            manifest_to_resume(manifest_url, manifest, target);
            save_resume_record();
            resume = false;
            goto retry_from_start;
        }
        return ESP_FAIL;
    }

    if (content_len < 0 || written + (size_t)content_len != manifest->size) {
        APP_LOGE(TAG, "firmware length mismatch written=%u content=%lld total=%u",
                 (unsigned)written, (long long)content_len, (unsigned)manifest->size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    err = resume ?
          esp_ota_resume(target, OTA_WITH_SEQUENTIAL_WRITES, written, &ota_handle) :
          esp_ota_begin(target, manifest->size, &ota_handle);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "ota begin/resume failed resume=%d", resume ? 1 : 0);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    set_state(OTA_SERVICE_STATE_DOWNLOADING, NULL);
    update_progress(written, manifest->size);

    while (written < manifest->size && !s_cancel_requested) {
        int r = esp_http_client_read(client, (char *)s_http_buf, OTA_HTTP_BUF_SIZE);
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            if (written >= manifest->size) {
                break;
            }
            err = ESP_FAIL;
            break;
        }

        err = esp_ota_write(ota_handle, s_http_buf, (size_t)r);
        if (err != ESP_OK) {
            APP_LOG_ERR(TAG, err, "ota write failed offset=%u", (unsigned)written);
            break;
        }

        written += (size_t)r;
        update_progress(written, manifest->size);

        if (written - last_saved >= OTA_SAVE_STEP_BYTES || written == manifest->size) {
            s_resume.written_size = written;
            save_resume_record();
            last_saved = written;
            APP_LOGI(TAG, "ota progress written=%u total=%u",
                     (unsigned)written, (unsigned)manifest->size);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (s_cancel_requested) {
        esp_ota_abort(ota_handle);
        return ESP_ERR_INVALID_STATE;
    }

    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        return err;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "ota end failed");
        return err;
    }

    return ESP_OK;
}

static esp_err_t verify_and_activate(const ota_manifest_t *manifest, const esp_partition_t *target)
{
    char actual_sha[OTA_SERVICE_SHA256_HEX_LEN];
    set_state(OTA_SERVICE_STATE_VERIFYING, NULL);

    esp_err_t err = sha256_partition(target, manifest->size, actual_sha);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "sha256 partition failed");
        return err;
    }

    APP_LOGI(TAG, "sha256 expected=%s actual=%s", manifest->sha256, actual_sha);
    if (strcasecmp(actual_sha, manifest->sha256) != 0) {
        return ESP_ERR_INVALID_CRC;
    }

    esp_app_desc_t new_desc;
    err = esp_ota_get_partition_description(target, &new_desc);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "get new app desc failed");
        return err;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    if (strcmp(new_desc.project_name, running->project_name) != 0) {
        APP_LOGE(TAG, "new app project mismatch new=%s running=%s",
                 new_desc.project_name, running->project_name);
        return ESP_ERR_INVALID_VERSION;
    }

    if (strcmp(new_desc.version, manifest->version) != 0) {
        APP_LOGE(TAG, "new app version mismatch new=%s manifest=%s",
                 new_desc.version, manifest->version);
        return ESP_ERR_INVALID_VERSION;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "set boot partition failed");
        return err;
    }

    clear_resume_record();
    lock_state();
    s_snapshot.state = OTA_SERVICE_STATE_READY_REBOOT;
    s_snapshot.busy = false;
    s_snapshot.progress = 100;
    s_snapshot.written_size = manifest->size;
    s_snapshot.total_size = manifest->size;
    s_snapshot.error[0] = '\0';
    strlcpy(s_snapshot.version, manifest->version, sizeof(s_snapshot.version));
    unlock_state();
    return ESP_OK;
}

static void ota_task(void *arg)
{
    char manifest_url[OTA_SERVICE_URL_MAX_LEN];
    bool resume_only = (arg != NULL);
    char *manifest_json = (char *)malloc(OTA_MANIFEST_MAX_LEN);
    ota_manifest_t manifest;
    esp_err_t err = ESP_OK;

    if (manifest_json == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    lock_state();
    strlcpy(manifest_url, s_pending_manifest_url, sizeof(manifest_url));
    unlock_state();

    if (resume_only) {
        if (!s_resume.valid || s_resume.manifest_url[0] == '\0') {
            err = ESP_ERR_INVALID_STATE;
            goto fail;
        }
        strlcpy(manifest_url, s_resume.manifest_url, sizeof(manifest_url));
    }

    set_state(OTA_SERVICE_STATE_FETCH_MANIFEST, NULL);
    err = http_get_to_buffer(manifest_url, manifest_json, OTA_MANIFEST_MAX_LEN);
    if (err != ESP_OK) {
        goto fail;
    }

    set_state(OTA_SERVICE_STATE_CHECK_MANIFEST, NULL);
    err = parse_manifest(manifest_json, &manifest);
    if (err != ESP_OK) {
        goto fail;
    }

    const esp_partition_t *target = NULL;
    bool resume_matches = s_resume.valid && manifest_matches_resume(manifest_url, &manifest);
    if (resume_matches) {
        target = find_partition_by_subtype(s_resume.partition_subtype);
    } else {
        if (s_resume.valid) {
            APP_LOGW(TAG, "manifest changed, clear old OTA resume state");
            clear_resume_record();
        }
        target = esp_ota_get_next_update_partition(NULL);
    }

    if (target == NULL) {
        err = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    if (!resume_matches) {
        manifest_to_resume(manifest_url, &manifest, target);
    }

    err = validate_manifest(&manifest, target);
    if (err != ESP_OK) {
        goto fail;
    }

    strlcpy(s_resume.manifest_url, manifest_url, sizeof(s_resume.manifest_url));
    s_resume.valid = true;
    save_resume_record();

    lock_state();
    strlcpy(s_snapshot.version, manifest.version, sizeof(s_snapshot.version));
    unlock_state();

    err = download_firmware(manifest_url, &manifest, target);
    if (err != ESP_OK) {
        goto fail;
    }

    err = verify_and_activate(&manifest, target);
    if (err != ESP_OK) {
        goto fail;
    }

    APP_LOGI(TAG, "OTA ready, version=%s", manifest.version);
    goto done;

fail:
    if (s_cancel_requested) {
        clear_resume_record();
        set_state(OTA_SERVICE_STATE_CANCELED, "canceled");
    } else {
        s_resume.fail_count++;
        if (s_resume.fail_count >= OTA_MAX_FAIL_COUNT) {
            APP_LOGW(TAG, "OTA fail count reached limit, clear resume state");
            clear_resume_record();
        } else if (s_resume.valid) {
            save_resume_record();
        }
        APP_LOG_ERR(TAG, err, "OTA failed");
        set_state(OTA_SERVICE_STATE_FAILED, esp_err_to_name(err));
    }

done:
    free(manifest_json);
    lock_state();
    s_task = NULL;
    s_cancel_requested = false;
    unlock_state();
    vTaskDelete(NULL);
}

esp_err_t ota_service_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    lock_state();
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = OTA_SERVICE_STATE_IDLE;
    s_snapshot.progress = 0;
    unlock_state();

    load_resume_record();
    return ESP_OK;
}

void ota_service_mark_app_valid_if_needed(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                APP_LOGI(TAG, "running app marked valid");
            } else {
                APP_LOG_ERR(TAG, err, "mark running app valid failed");
            }
        }
    }
}

void ota_service_notify_wifi_state(void)
{
    if (wifi_manager_is_sta_connected()) {
        ota_service_resume();
    }
}

static esp_err_t start_task(const char *manifest_url, bool resume_only)
{
    if (!resume_only && (!starts_with_https(manifest_url))) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_state();
    if (s_task != NULL || s_snapshot.busy) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    if (!resume_only) {
        strlcpy(s_pending_manifest_url, manifest_url, sizeof(s_pending_manifest_url));
    }
    s_cancel_requested = false;
    BaseType_t ok = xTaskCreate(ota_task,
                                "ota_service",
                                OTA_TASK_STACK_SIZE,
                                resume_only ? (void *)1 : NULL,
                                OTA_TASK_PRIORITY,
                                &s_task);
    unlock_state();
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ota_service_start(const char *manifest_url)
{
    return start_task(manifest_url, false);
}

esp_err_t ota_service_resume(void)
{
    if (!wifi_manager_is_sta_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_resume.valid) {
        return ESP_ERR_INVALID_STATE;
    }
    return start_task(NULL, true);
}

esp_err_t ota_service_cancel(void)
{
    lock_state();
    if (s_task != NULL || s_snapshot.busy) {
        s_cancel_requested = true;
        unlock_state();
        return ESP_OK;
    }
    unlock_state();

    clear_resume_record();
    set_state(OTA_SERVICE_STATE_CANCELED, "canceled");
    return ESP_OK;
}

esp_err_t ota_service_reboot_if_ready(void)
{
    lock_state();
    bool ready = (s_snapshot.state == OTA_SERVICE_STATE_READY_REBOOT);
    unlock_state();
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_restart();
    return ESP_OK;
}

void ota_service_get_snapshot(ota_service_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    lock_state();
    *out = s_snapshot;
    unlock_state();
}

const char *ota_service_state_to_string(ota_service_state_t state)
{
    switch (state) {
    case OTA_SERVICE_STATE_IDLE: return "IDLE";
    case OTA_SERVICE_STATE_FETCH_MANIFEST: return "FETCH_MANIFEST";
    case OTA_SERVICE_STATE_CHECK_MANIFEST: return "CHECK_MANIFEST";
    case OTA_SERVICE_STATE_DOWNLOADING: return "DOWNLOADING";
    case OTA_SERVICE_STATE_VERIFYING: return "VERIFYING";
    case OTA_SERVICE_STATE_READY_REBOOT: return "READY_REBOOT";
    case OTA_SERVICE_STATE_FAILED: return "FAILED";
    case OTA_SERVICE_STATE_CANCELED: return "CANCELED";
    default: return "UNKNOWN";
    }
}

int ota_service_format_status_json(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return -1;
    }

    ota_service_snapshot_t snap;
    ota_service_get_snapshot(&snap);
    int len = snprintf(out,
                       out_size,
                       "\"ota_state\":\"%s\",\"ota_progress\":%d,\"ota_version\":\"%s\","
                       "\"ota_written\":%u,\"ota_total\":%u,\"ota_error\":\"%s\","
                       "\"ota_resume_valid\":%d",
                       ota_service_state_to_string(snap.state),
                       snap.progress,
                       snap.version,
                       (unsigned)snap.written_size,
                       (unsigned)snap.total_size,
                       snap.error,
                       snap.resume_valid ? 1 : 0);
    return (len > 0 && (size_t)len < out_size) ? len : -1;
}

int ota_service_format_status_object_json(char *out, size_t out_size)
{
    if (out == NULL || out_size < 3) {
        return -1;
    }

    out[0] = '{';
    int len = ota_service_format_status_json(out + 1, out_size - 2);
    if (len < 0 || (size_t)len + 2 >= out_size) {
        return -1;
    }
    out[len + 1] = '}';
    out[len + 2] = '\0';
    return len + 2;
}
