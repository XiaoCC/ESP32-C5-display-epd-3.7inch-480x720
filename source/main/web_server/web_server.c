#include "cJSON.h"
#include "web_server.h"
#include "pin_map.h"
#include "tf_card.h"
#include "screen_rotation.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "axp2101.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "screen_driver.h"
#include "wifi_mgr.h"
#include "lwip/sockets.h"
#include "app_display.h"
#include "dashboard.h"
#include "page_data.h"

static const char *TAG = "web";
static TickType_t s_last_activity;
static bool s_user_request_active;

void web_server_note_activity(void)
{
    __atomic_store_n(&s_last_activity, xTaskGetTickCount(), __ATOMIC_RELAXED);
}

uint32_t web_server_idle_ms(void)
{
    if (__atomic_load_n(&s_user_request_active, __ATOMIC_ACQUIRE)) return 0;
    return pdTICKS_TO_MS(xTaskGetTickCount() -
        __atomic_load_n(&s_last_activity, __ATOMIC_RELAXED));
}

typedef enum { JOB_IDLE, JOB_REFRESH, JOB_RECEIVING, JOB_ERROR } job_phase_t;
typedef enum { JOB_FRAME, JOB_CLEAR_SCREEN, JOB_ALBUM_SHOW } job_kind_t;
#define FRAME_FORMAT "otp6-v1"
#define FRAME_CHUNK_BYTES 2048

static portMUX_TYPE s_job_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile job_phase_t s_job_phase = JOB_IDLE;
static esp_err_t s_job_err = ESP_OK;
static TaskHandle_t s_job_task;
static uint32_t s_job_id;
static job_kind_t s_job_kind;
static screen_upload_report_t s_upload_report;
static const char *s_job_step = "idle";
static size_t s_job_bytes;
typedef struct {
    httpd_req_t *req;
    size_t bytes;
    TickType_t started;
    uint32_t hash, expected;
    uint32_t id, base_generation, rotation_epoch;
    screen_mode_t mode;
    bool save_to_album;
    bool mono;
} frame_rx_t;
static frame_rx_t s_pending;
static char s_photo_pending[13];

static void log_heap_stats(const char *event, uint32_t id)
{
    const uint32_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t min8 = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    const uint32_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "heap %s id=%u free8=%u min8=%u largest8=%u internal=%u internal_largest=%u",
             event, (unsigned)id, (unsigned)free8, (unsigned)min8, (unsigned)largest8,
             (unsigned)free_internal, (unsigned)largest_internal);
}


static esp_err_t send_json(httpd_req_t *req, const char *json);
static void frame_job_task(void *arg);
static const char *job_kind_name(job_kind_t kind)
{
    if (kind == JOB_ALBUM_SHOW) return "album-show";
    return kind == JOB_CLEAR_SCREEN ? "clear-screen" : "frame";
}
static const char *job_phase_name(job_phase_t phase)
{
    switch (phase) {
    case JOB_REFRESH: return "refresh";
    case JOB_RECEIVING: return "receiving";
    case JOB_ERROR: return "error";
    default: return "idle";
    }
}

bool web_server_screen_busy(void){return s_job_phase == JOB_RECEIVING || s_job_phase == JOB_REFRESH;}
static bool begin_job(job_kind_t kind)
{
    portENTER_CRITICAL(&s_job_lock);
    bool available = !screen_rotation_busy() && (s_job_phase == JOB_IDLE || s_job_phase == JOB_ERROR);
    if (available) {
        if (++s_job_id == 0) ++s_job_id;
        s_job_kind = kind;
        s_job_err = ESP_OK;
        s_job_phase = JOB_RECEIVING;
        s_job_step = "queued";
        s_job_bytes = 0;
        memset(&s_upload_report, 0, sizeof(s_upload_report));
    }
    portEXIT_CRITICAL(&s_job_lock);
    return available;
}
static void finish_job(esp_err_t err, const screen_upload_report_t *report)
{
    portENTER_CRITICAL(&s_job_lock);
    s_upload_report = *report;
    s_job_err = err;
    s_job_phase = err == ESP_OK ? JOB_IDLE : JOB_ERROR;
    s_job_step = err == ESP_OK ? "done" : "error";
    portEXIT_CRITICAL(&s_job_lock);
}

/* Append one contract to both job polling and the asynchronous OTP result.
 * Zero hashes mean no complete successful source transmission was recorded. */
static esp_err_t send_report_json(httpd_req_t *req, char *json, size_t capacity, int used,
                                  const screen_upload_report_t *report, const char *sequence)
{
    if (used < 0 || (size_t)used >= capacity) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "job JSON overflow");
        return ESP_FAIL;
    }
    const int written = snprintf(json + used, capacity - (size_t)used,
        "\"sequence\":\"%s\",\"plane_fnv\":[\"%08x\"],\"completed_mask\":%u}",
        sequence, (unsigned)report->frame_fnv, report->completed ? 1U : 0U);
    if (written < 0 || (size_t)written >= capacity - (size_t)used) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "job JSON overflow");
        return ESP_FAIL;
    }
    return send_json(req, json);
}
static esp_err_t send_job_json(httpd_req_t *req)
{
    char json[512];
    portENTER_CRITICAL(&s_job_lock);
    const uint32_t id = s_job_id;
    const job_phase_t phase = s_job_phase;
    const esp_err_t err = s_job_err;
    const job_kind_t kind = s_job_kind;
    const char *step = s_job_step;
    const size_t bytes = s_job_bytes;
    const screen_upload_report_t report = s_upload_report;
    portEXIT_CRITICAL(&s_job_lock);
    const int used = snprintf(json, sizeof(json),
        "{\"ok\":true,\"id\":%u,\"phase\":\"%s\",\"err\":%d,\"kind\":\"%s\","
        "\"step\":\"%s\",\"bytes\":%u,\"total_ms\":%u,\"refresh_ms\":%u,"
        "\"display_generation\":%u,\"current_display_generation\":%u,",
        (unsigned)id, job_phase_name(phase), (int)err, job_kind_name(kind), step,
        (unsigned)bytes, (unsigned)report.total_ms, (unsigned)report.refresh_ms,
        (unsigned)report.display_generation, (unsigned)screen_driver_generation());
    return send_report_json(req, json, sizeof(json), used, &report,
        kind == JOB_CLEAR_SCREEN ? "otp6-clear-v1" : FRAME_FORMAT);
}

extern const uint8_t index_html_start[]        asm("_binary_index_html_start");
extern const uint8_t index_html_end[]          asm("_binary_index_html_end");
extern const uint8_t pipeline_js_start[]       asm("_binary_eink_pipeline_js_start");
extern const uint8_t pipeline_js_end[]         asm("_binary_eink_pipeline_js_end");
extern const uint8_t vue_js_start[]            asm("_binary_vue_global_prod_js_start");
extern const uint8_t vue_js_end[]              asm("_binary_vue_global_prod_js_end");

static esp_err_t send_embedded(httpd_req_t *req, const uint8_t *start, const uint8_t *end,
                               const char *type, const char *cache)
{
    const size_t len = (size_t)(end - start);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", cache ? cache : "no-cache");
    /* A failed response must propagate so IDF can retire the bad session. */
    return httpd_resp_send(req, (const char *)start, len);
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP response failed uri=%s fd=%d err=%s", req->uri,
                 httpd_req_to_sockfd(req), esp_err_to_name(err));
    }
    return err;
}

extern const uint8_t weather_start[] asm("_binary_weather_js_start");
extern const uint8_t weather_end[] asm("_binary_weather_js_end");

static esp_err_t weather_script_get(httpd_req_t *req)
{
    return send_embedded(req, weather_start, weather_end, "application/javascript; charset=utf-8", "no-store");
}

static esp_err_t read_json_body(httpd_req_t *req, char *body, size_t capacity)
{
    if (req->content_len == 0 || req->content_len >= capacity) return ESP_ERR_INVALID_SIZE;
    size_t used = 0;
    while (used < req->content_len) {
        const int received = httpd_req_recv(req, body + used, req->content_len - used);
        if (received <= 0) return ESP_FAIL;
        used += received;
    }
    if (memchr(body, 0, used)) return ESP_ERR_INVALID_ARG;
    body[used] = 0;
    return ESP_OK;
}

static esp_err_t weather_settings_get(httpd_req_t *req)
{
    char settings[WEATHER_LOCATION_BYTES];
    const esp_err_t err = dashboard_location_get(settings, sizeof(settings));
    return send_json(req, err == ESP_OK ? settings : dashboard_default_location());
}

static esp_err_t weather_settings_post(httpd_req_t *req)
{
    char body[WEATHER_LOCATION_BYTES];
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid location request");
    }
    const esp_err_t err = dashboard_location_set(body);
    if (err == ESP_ERR_INVALID_ARG) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid location");
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "location save failed");
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t schedule_get(httpd_req_t *req)
{
    char buf[384];
    dashboard_schedule_t schedule;
    dashboard_schedule_get(&schedule);
    snprintf(buf, sizeof(buf), "{\"enabled\":%u,\"running\":%s,\"every\":%d}",
        (unsigned)schedule.enabled, schedule.running ? "true" : "false", schedule.every);
    return send_json(req, buf);
}

static bool parse_minutes(const cJSON *item, int low, int16_t *value)
{
    if (!cJSON_IsNumber(item) || item->valuedouble != item->valueint ||
        item->valueint < low || item->valueint > 1439) return false;
    *value = item->valueint;
    return true;
}

static esp_err_t schedule_post(httpd_req_t *req)
{
    char body[384];
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid schedule request");
    }
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON *running = cJSON_GetObjectItemCaseSensitive(root, "running");
    dashboard_schedule_t schedule = {0};
    const bool valid = cJSON_IsNumber(enabled) && enabled->valueint > 0 &&
        enabled->valueint < (1 << SCREEN_MODE_COUNT) && enabled->valuedouble == enabled->valueint &&
        cJSON_IsBool(running) &&
        parse_minutes(cJSON_GetObjectItemCaseSensitive(root, "every"), 1, &schedule.every);
    if (valid) {
        schedule.enabled = enabled->valueint;
        schedule.running = cJSON_IsTrue(running);
    }
    cJSON_Delete(root);
    const esp_err_t err = valid ? dashboard_schedule_set(&schedule) : ESP_ERR_INVALID_ARG;
    if (err == ESP_ERR_INVALID_ARG) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid schedule");
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "schedule save failed");
    app_display_schedule_changed();
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t wifi_off_post(httpd_req_t *req)
{
    const esp_err_t err = send_json(req, "{\"ok\":true}");
    if (err == ESP_OK) app_display_wifi_off();
    return err;
}

static esp_err_t page_write_result(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_OK) return send_json(req, "{\"ok\":true}");
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"ok\":false,\"message\":\"请检查必填字段和日期格式。\"}");
    }
    httpd_resp_set_status(req, "503 Service Unavailable");
    return send_json(req, "{\"ok\":false,\"message\":\"SD 卡不可写，请检查 SD 卡。\"}");
}

static esp_err_t trips_handler(httpd_req_t *req)
{
    char *body = malloc(TRIPS_BYTES);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    esp_err_t result;
    if (req->method == HTTP_POST) {
        const esp_err_t err = read_json_body(req, body, TRIPS_BYTES) == ESP_OK ?
            page_trips_set(body) : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK) page_network_refresh();
        result = page_write_result(req, err);
    } else {
        const esp_err_t err = page_sd_read("TRIPS.JSN", body, TRIPS_BYTES);
        if (err == ESP_OK) {
            char *status = page_trips_with_status(body);
            result = status ? send_json(req, status) :
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not read trips");
            free(status);
        } else result = err == ESP_ERR_NOT_FOUND ? send_json(req, "[]") : page_write_result(req, err);
    }
    free(body);
    return result;
}

static esp_err_t trip_select_post(httpd_req_t *req)
{
    char id[10];
    if (read_json_body(req, id, sizeof(id)) != ESP_OK) return page_write_result(req, ESP_ERR_INVALID_ARG);
    const esp_err_t err = page_trip_select(id);
    if (err != ESP_OK) return page_write_result(req, err);
    return send_json(req, app_display_request(SCREEN_MODE_TRIP) ?
        "{\"ok\":true,\"queued\":true}" : "{\"ok\":true,\"queued\":false}");
}

static esp_err_t shortcut_handler(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        char body[4];
        if (read_json_body(req, body, sizeof(body)) != ESP_OK || strlen(body) != 1 ||
                body[0] < '0' || body[0] >= '0' + SCREEN_MODE_COUNT)
            return page_write_result(req, ESP_ERR_INVALID_ARG);
        const esp_err_t err = page_shortcut_set(body[0] - '0');
        if (err != ESP_OK) return page_write_result(req, err);
    }
    char json[40];
    snprintf(json, sizeof(json), "{\"ok\":true,\"mode\":%u}", (unsigned)page_shortcut_get());
    return send_json(req, json);
}

static esp_err_t lunar_post(httpd_req_t *req)
{
    if (req->content_len >= LUNAR_BYTES)
        return page_write_result(req, ESP_ERR_INVALID_SIZE);
    char *body = malloc(req->content_len + 1);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    const esp_err_t err = read_json_body(req, body, req->content_len + 1) == ESP_OK ?
        page_lunar_upload(body) : ESP_ERR_INVALID_ARG;
    free(body);
    return page_write_result(req, err);
}

static esp_err_t place_catalog_get(httpd_req_t *req)
{
    extern const char catalog_start[] asm("_binary_place_catalog_gz_start");
    extern const char catalog_end[] asm("_binary_place_catalog_gz_end");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, catalog_start, catalog_end - catalog_start);
}

static esp_err_t lunar_script(httpd_req_t *req)
{
    extern const char lunar_script_start[] asm("_binary_lunar_web_gz_start");
    extern const char lunar_script_end[] asm("_binary_lunar_web_gz_end");
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, lunar_script_start, lunar_script_end - lunar_script_start);
}

static esp_err_t history_get(httpd_req_t *req)
{
    struct tm day;
    if (!dashboard_local_time(&day)) return send_json(req, "{\"items\":[]}");
    cJSON *root = cJSON_CreateObject();
    cJSON *items = root ? cJSON_AddArrayToObject(root, "items") : NULL;
    if (!items) { cJSON_Delete(root); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); }
    for (int i = 0; i < 7; ++i) {
        page_history_t item;
        cJSON *entry = cJSON_CreateObject();
        if (!entry) break;
        char date[11];
        strftime(date, sizeof(date), "%Y-%m-%d", &day);
        cJSON_AddStringToObject(entry, "date", date);
        if (page_history_get(&day, &item)) {
            cJSON_AddNumberToObject(entry, "year", item.year);
            cJSON_AddStringToObject(entry, "text", item.text);
        }
        cJSON_AddItemToArray(items, entry);
        ++day.tm_mday;
        mktime(&day);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    const esp_err_t err = json ? send_json(req, json) : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    free(json);
    return err;
}

static esp_err_t display_style(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        char body[64];
        if (read_json_body(req, body, sizeof(body)) != ESP_OK)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid display style");
        cJSON *json = cJSON_ParseWithOpts(body, NULL, true);
        const cJSON *mask = cJSON_GetObjectItemCaseSensitive(json, "mono");
        const bool valid = cJSON_IsNumber(mask) && mask->valuedouble == mask->valueint &&
            mask->valueint >= 0 && mask->valueint < (1 << SCREEN_MODE_COUNT);
        const esp_err_t err = valid ? dashboard_mono_set(mask->valueint) : ESP_ERR_INVALID_ARG;
        cJSON_Delete(json);
        if (err == ESP_ERR_INVALID_ARG)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid display style");
        if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not save display style");
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"mono\":%u,\"fast_refresh\":false}",
        (unsigned)dashboard_mono_mask());
    return send_json(req, json);
}

static esp_err_t weather_policy(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        char body[8];
        if (read_json_body(req, body, sizeof(body)) != ESP_OK ||
                (strcmp(body, "true") && strcmp(body, "false")))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid weather policy");
        if (dashboard_weather_set_offline(!strcmp(body, "true")) != ESP_OK)
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not save weather policy");
    }
    return send_json(req, dashboard_weather_offline() ? "{\"offline\":true}" : "{\"offline\":false}");
}

static esp_err_t quotes_get(httpd_req_t *req)
{
    char *content = NULL;
    size_t length = 0;
    const esp_err_t err = dashboard_quote_export(&content, &length);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_json(req, "{\"ok\":false,\"message\":\"无法读取 SD 卡句库。\"}");
    }
    httpd_resp_set_type(req, "text/tab-separated-values; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t response = httpd_resp_send(req, content, length);
    free(content);
    return response;
}

static esp_err_t quotes_post(httpd_req_t *req)
{
    char *content = malloc(QUOTE_IMPORT_BYTES);
    if (!content) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    const bool received = read_json_body(req, content, QUOTE_IMPORT_BYTES) == ESP_OK;
    unsigned bad_row = 0;
    const esp_err_t err = received ? dashboard_quote_import(content, &bad_row) : ESP_ERR_INVALID_SIZE;
    free(content);
    if (err == ESP_OK) return send_json(req, "{\"ok\":true}");
    httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE ? "503 Service Unavailable" :
        err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE ? "400 Bad Request" : "500 Internal Server Error");
    if (err == ESP_ERR_INVALID_ARG) {
        char json[160];
        snprintf(json, sizeof(json), "{\"ok\":false,\"message\":\"第 %u 条格式有误、内容过长或包含屏幕无法显示的字符。\"}", bad_row);
        return send_json(req, json);
    }
    if (err == ESP_ERR_INVALID_SIZE) return send_json(req, "{\"ok\":false,\"message\":\"导入内容或 SD 句库超过容量。\"}");
    return send_json(req, "{\"ok\":false,\"message\":\"无法保存句库，请检查 SD 卡。\"}");
}

static esp_err_t quote_show_post(httpd_req_t *req)
{
    char content[512];
    if (read_json_body(req, content, sizeof(content)) != ESP_OK ||
            dashboard_quote_preview(content) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"ok\":false,\"message\":\"摘句格式有误、内容过长或包含屏幕无法显示的字符。\"}");
    }
    if (!app_display_request(SCREEN_MODE_QUOTE)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_json(req, "{\"ok\":false,\"message\":\"屏幕任务暂不可用。\"}");
    }
    httpd_resp_set_status(req, "202 Accepted");
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t todo_get(httpd_req_t *req)
{
    char *body = malloc(TODO_JSON_BYTES);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    const esp_err_t err = dashboard_todo_get(body, TODO_JSON_BYTES);
    esp_err_t result;
    if (err == ESP_OK) result = send_json(req, body);
    else if (err == ESP_ERR_NOT_FOUND) result = send_json(req, "{\"items\":[]}");
    else result = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not read tasks");
    free(body);
    return result;
}

static esp_err_t todo_post(httpd_req_t *req)
{
    char *body = malloc(TODO_JSON_BYTES);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    const bool received = read_json_body(req, body, TODO_JSON_BYTES) == ESP_OK;
    const esp_err_t err = received ? dashboard_todo_set(body) : ESP_ERR_INVALID_ARG;
    free(body);
    if (err == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid task list");
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not save tasks");
    const bool queued = app_display_request(SCREEN_MODE_TODO);
    return send_json(req, queued ? "{\"saved\":true,\"queued\":true}" : "{\"saved\":true,\"queued\":false}");
}

static esp_err_t refresh_mode_post(httpd_req_t *req)
{
    char body[24];
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid display request");
    }
    screen_mode_t mode;
    if (strcmp(body, "text") == 0) mode = SCREEN_MODE_TEXT;
    else if (strcmp(body, "photo") == 0) mode = SCREEN_MODE_PHOTO;
    else if (strcmp(body, "weather") == 0) mode = SCREEN_MODE_WEATHER;
    else if (strcmp(body, "calendar") == 0) mode = SCREEN_MODE_CALENDAR;
    else if (strcmp(body, "quote") == 0) mode = SCREEN_MODE_QUOTE;
    else if (strcmp(body, "todo") == 0) mode = SCREEN_MODE_TODO;
    else if (strcmp(body, "trip") == 0) mode = SCREEN_MODE_TRIP;
    else if (strcmp(body, "history") == 0) mode = SCREEN_MODE_HISTORY;
    else if (strcmp(body, "weather-update") == 0) mode = SCREEN_MODE_WEATHER;
    else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown display mode");
    if (!(screen_rotation_available_modes() & (1U << mode))) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"message\":\"请先发送这页的内容\"}");
    }
    const bool queued = strcmp(body, "weather-update") == 0
        ? app_display_refresh_weather() : app_display_request(mode);
    if (!queued) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_json(req, "{\"ok\":false,\"message\":\"screen task unavailable\"}");
    }
    httpd_resp_set_status(req, "202 Accepted");
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return send_embedded(req, index_html_start, index_html_end, "text/html; charset=utf-8", "no-cache");
}

static esp_err_t vue_get_handler(httpd_req_t *req)
{
    return send_embedded(req, vue_js_start, vue_js_end,
                         "application/javascript; charset=utf-8", "public, max-age=86400");
}

static esp_err_t pipeline_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)pipeline_js_start,
                                        pipeline_js_end - pipeline_js_start);
    if (err != ESP_OK) return err;
    char config[64];
    int n = snprintf(config, sizeof(config), ";EinkPipeline.setPanelRotation(%u);", screen_rotation_get());
    if (n < 0 || (size_t)n >= sizeof(config)) return ESP_FAIL;
    err = httpd_resp_send_chunk(req, config, n);
    if (err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void escape_ssid(const char *src, char *dst)
{
    static const char hex[] = "0123456789abcdef";
    while (*src) {
        unsigned char c = (unsigned char)*src++;
        if (c < 32 || c == 34 || c == 92) {
            *dst++ = 92; *dst++ = 'u'; *dst++ = '0'; *dst++ = '0';
            *dst++ = hex[c >> 4]; *dst++ = hex[c & 15];
        } else *dst++ = (char)c;
    }
    *dst = 0;
}

static size_t append_charge_json(char *json, size_t capacity, size_t used);

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char ip[16] = { 0 };
    char ssid[33] = { 0 };
    char escaped_ssid[193];
    char url[48] = { 0 };
    char json[960];
    wifi_mgr_get_ip(ip, sizeof(ip));
    wifi_mgr_get_ssid(ssid, sizeof(ssid));
    wifi_mgr_service_url(url, sizeof(url));
    escape_ssid(ssid, escaped_ssid);
    int n = snprintf(json, sizeof(json),
        "{\"ok\":true,\"format\":\"otp6-v1\",\"w\":480,\"h\":720,\"frame\":172800,"
        "\"mode\":\"%s\",\"internet_confirmed\":%s,\"ip\":\"%s\",\"ssid\":\"%s\",\"url\":\"%s\"",
        wifi_mgr_is_sta_up() ? "sta" : "ap", wifi_mgr_internet_confirmed() ? "true" : "false",
        ip, escaped_ssid, url);
    if (n < 0 || (size_t)n >= sizeof(json)) return ESP_FAIL;
    size_t used = append_charge_json(json, sizeof(json), (size_t)n);
    if (used + 48 >= sizeof(json)) return ESP_FAIL;
    struct tm local;
    char clock[24] = "";
    if (dashboard_local_time(&local)) strftime(clock, sizeof(clock), "%Y-%m-%d %H:%M:%S", &local);
    static const char *const screens[] = {"text", "photo", "calendar", "weather", "quote", "todo", "trip", "history", "network"};
    const screen_mode_t current = screen_rotation_current_mode();
    int tail = snprintf(json + used, sizeof(json) - used,
        ",\"job\":\"%s\",\"job_id\":%u,\"rotation\":%u,\"rotation_epoch\":%u,\"rotation_busy\":%s,\"active_screen\":\"%s\",\"clock\":\"%s\",\"weather_data\":\"%s\",\"weather_source\":\"%s\",\"available_screens\":%u}",
        job_phase_name(s_job_phase), (unsigned)s_job_id, screen_rotation_get(), (unsigned)screen_rotation_epoch(), screen_rotation_busy() ? "true" : "false",
        screens[current], clock, dashboard_weather_ready() ? "ready" : "pending",
        dashboard_weather_ready() ? (dashboard_weather_is_live() ? "live" : "cache") : "none",
        (unsigned)screen_rotation_available_modes());
    if (tail < 0 || (size_t)tail >= sizeof(json) - used) return ESP_FAIL;
    return send_json(req, json);
}




static size_t append_charge_json(char *json, size_t capacity, size_t used)
{
    axp2101_chg_pwr_t pwr = axp2101_get_charge_power();
    bool enabled = false, present = false;
    int soc = -1;
    bool valid = axp2101_gauge_status(&enabled, &present, &soc) == ESP_OK;
    int n = snprintf(json + used, capacity - used,
        ",\"charge\":\"%s\",\"name\":\"%s\",\"capacity_mah\":%u,\"target_ma\":%u,\"icc_ma\":%u,\"vbus_lim_ma\":%u,\"gauge_read_ok\":%s,\"gauge_enabled\":%s,\"battery_present\":%s,\"soc\":%d,\"low_battery\":%s,\"learning_completion_available\":false",
        axp2101_charge_power_id(pwr), axp2101_charge_power_name(pwr),
        (unsigned)axp2101_capacity_mah(), (unsigned)axp2101_charge_target_ma(pwr),
        (unsigned)axp2101_charge_icc_ma(pwr), (unsigned)axp2101_charge_vbus_lim_ma(pwr),
        valid ? "true" : "false", enabled ? "true" : "false", present ? "true" : "false", soc,
        axp2101_low_battery_percent() >= 0 ? "true" : "false");
    if (n < 0 || (size_t)n >= capacity - used) return capacity;
    return used + (size_t)n;
}

static void send_charge_json(httpd_req_t *req)
{
    char json[384] = "{\"ok\":true";
    size_t used = append_charge_json(json, sizeof(json), strlen(json));
    if (used + 2 >= sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "charge response overflow");
        return;
    }
    json[used++] = '}';
    json[used] = 0;
    (void)send_json(req, json);
}

static esp_err_t charge_get_handler(httpd_req_t *req)
{
    send_charge_json(req);
    return ESP_OK;
}

static esp_err_t charge_post_handler(httpd_req_t *req)
{
    char body[128];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body size");
        return ESP_FAIL;
    }
    size_t used = 0;
    while (used < req->content_len) {
        int n = httpd_req_recv(req, body + used, req->content_len - used);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete charge request");
            return ESP_FAIL;
        }
        used += (size_t)n;
    }
    body[used] = 0;
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "power");
    const cJSON *cap = cJSON_GetObjectItemCaseSensitive(root, "capacity_mah");
    int pwr = -1;
    if (cJSON_IsString(mode)) {
        if (!strcmp(mode->valuestring, "slow") || !strcmp(mode->valuestring, "1w") || !strcmp(mode->valuestring, "1")) pwr = 0;
        if (!strcmp(mode->valuestring, "balanced") || !strcmp(mode->valuestring, "2.5w") || !strcmp(mode->valuestring, "2.5")) pwr = 1;
        if (!strcmp(mode->valuestring, "fast") || !strcmp(mode->valuestring, "5w") || !strcmp(mode->valuestring, "5")) pwr = 2;
    }
    double capacity = cap && cJSON_IsNumber(cap) ? cap->valuedouble : axp2101_capacity_mah();
    bool valid = cJSON_IsObject(root) && pwr >= 0 && (!cap || cJSON_IsNumber(cap)) &&
                 capacity >= 500 && capacity <= 10000 && capacity == (uint16_t)capacity;
    cJSON_Delete(root);
    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "power or capacity_mah (integer 500..10000)");
        return ESP_FAIL;
    }
    esp_err_t err = axp2101_set_charge_config((axp2101_chg_pwr_t)pwr, (uint16_t)capacity);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    send_charge_json(req);
    return ESP_OK;
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    static char json[2048];
    (void)wifi_mgr_scan_json(json, sizeof(json));
    return send_json(req, json);
}

static esp_err_t wifi_connect_handler(httpd_req_t *req)
{
    char body[256];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wifi request size");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete wifi request");
            return ESP_FAIL;
        }
        received += (size_t)n;
    }
    body[received] = 0;
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *ssid_json = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass_json = cJSON_GetObjectItemCaseSensitive(root, "password");
    const bool valid = cJSON_IsObject(root) && cJSON_IsString(ssid_json) &&
        ssid_json->valuestring[0] && strlen(ssid_json->valuestring) <= 32 &&
        (!pass_json || (cJSON_IsString(pass_json) && strlen(pass_json->valuestring) <= 64));
    if (!valid) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ssid or password");
        return ESP_FAIL;
    }
    char ssid[33], pass[65] = {0};
    memcpy(ssid, ssid_json->valuestring, strlen(ssid_json->valuestring) + 1);
    if (pass_json) memcpy(pass, pass_json->valuestring, strlen(pass_json->valuestring) + 1);
    cJSON_Delete(root);

    esp_err_t err = wifi_mgr_connect(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    send_json(req, "{\"ok\":true,\"msg\":\"connecting\"}");
    return ESP_OK;
}

/* Validate each six-ink chunk before writing it to controller RAM. */
static esp_err_t receive_frame_chunk(void *context, uint8_t *buffer,
                                     size_t capacity, size_t *received)
{
    frame_rx_t *rx = context;
    *received = 0;
    if (rx->bytes >= SCREEN_FRAME_BYTES) return ESP_ERR_INVALID_SIZE;
    if (xTaskGetTickCount() - rx->started > pdMS_TO_TICKS(60000)) return ESP_ERR_TIMEOUT;
    size_t remaining = SCREEN_FRAME_BYTES - rx->bytes;
    if (capacity > remaining) capacity = remaining;
    int ret = HTTPD_SOCK_ERR_TIMEOUT;
    for (unsigned attempt = 0; attempt < 3 && ret == HTTPD_SOCK_ERR_TIMEOUT; ++attempt) {
        ret = httpd_req_recv(rx->req, (char *)buffer, capacity);
    }
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) return ESP_ERR_TIMEOUT;
    if (ret <= 0) return ESP_FAIL;
    for (int i = 0; i < ret; ++i) {
        uint8_t hi = buffer[i] >> 4, lo = buffer[i] & 15;
        bool valid = ((hi <= 3 || hi == 5 || hi == 6) && (lo <= 3 || lo == 5 || lo == 6));
        if (!valid) return ESP_ERR_INVALID_ARG;
        rx->hash = (rx->hash ^ buffer[i]) * 16777619U;
    }
    (void)screen_rotation_capture_write(rx->bytes, buffer, ret);
    if (rx->mono) {
        for (int i = 0; i < ret; ++i) buffer[i] = screen_driver_mono_pair(buffer[i], rx->bytes + i);
    }
    rx->bytes += ret;
    portENTER_CRITICAL(&s_job_lock);
    s_job_bytes = rx->bytes;
    portEXIT_CRITICAL(&s_job_lock);
    if (rx->bytes == SCREEN_FRAME_BYTES) {
        ESP_LOGI(TAG, "frame check id=%u bytes=%u hash=%08x expected=%08x",
                 (unsigned)rx->id, (unsigned)rx->bytes, (unsigned)rx->hash,
                 (unsigned)rx->expected);
        if (rx->hash != rx->expected) return ESP_ERR_INVALID_CRC;
    }
    *received = ret;
    return ESP_OK;
}
static esp_err_t upload_progress(void *context, screen_upload_phase_t phase)
{
    frame_rx_t *rx = context;
    const bool receiving = phase == SCREEN_UPLOAD_RECEIVING;
    if (receiving) rx->started = xTaskGetTickCount();
    portENTER_CRITICAL(&s_job_lock);
    s_job_phase = receiving ? JOB_RECEIVING : JOB_REFRESH;
    s_job_step = receiving ? "receiving" : "refresh";
    portEXIT_CRITICAL(&s_job_lock);
    ESP_LOGI(TAG, "frame id=%u step=%s", (unsigned)rx->id, receiving ? "receiving" : "refresh");
    return ESP_OK;
}

static void frame_job_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const job_kind_t kind = s_job_kind;
        frame_rx_t rx = s_pending;
        screen_upload_report_t report = {0};
        esp_err_t err;
        screen_rotation_lock();
        if (kind == JOB_FRAME) {
            rx.mono = (dashboard_mono_mask() & (1U << rx.mode)) != 0;
            const uint8_t blank[3] = {1,1,1};
            (void)screen_rotation_capture_begin(SCREEN_CACHE_OTP6, blank, rx.mode);
            uint8_t *buffer = malloc(FRAME_CHUNK_BYTES);
            err = !screen_rotation_matches(rx.rotation_epoch) ? ESP_ERR_INVALID_STATE :
                !buffer ? ESP_ERR_NO_MEM : screen_driver_upload(rx.base_generation,
                buffer, FRAME_CHUNK_BYTES, receive_frame_chunk, &rx, upload_progress, &report);
            free(buffer);
            const esp_err_t stored = screen_rotation_capture_end(err == ESP_OK, report.display_generation, rx.save_to_album);
            if (rx.mono && err == ESP_OK) report.frame_fnv = rx.hash;
            if (err == ESP_OK) err = stored;
            ESP_LOGI(TAG, "frame id=%u end %s bytes=%u",
                     (unsigned)rx.id, esp_err_to_name(err), (unsigned)rx.bytes);
            log_heap_stats("frame-end", rx.id);
            /* Publish the completed baseline before the browser receives its
             * receipt and can immediately verify or reuse that baseline. */
            finish_job(err, &report);
            if (err == ESP_OK) {
                char json[384];
                const int used = snprintf(json, sizeof(json),
                    "{\"ok\":true,\"id\":%u,\"phase\":\"idle\",\"err\":0,\"kind\":\"frame\","
                    "\"total_ms\":%u,\"refresh_ms\":%u,\"display_generation\":%u,",
                    (unsigned)rx.id, (unsigned)report.total_ms,
                    (unsigned)report.refresh_ms, (unsigned)report.display_generation);
                (void)send_report_json(rx.req, json, sizeof(json), used, &report, FRAME_FORMAT);
            } else {
                httpd_resp_send_err(rx.req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
                /* Discard any unread body before the async request releases its socket. */
                shutdown(httpd_req_to_sockfd(rx.req), SHUT_RDWR);
            }
            const esp_err_t complete = httpd_req_async_handler_complete(rx.req);
            if (complete != ESP_OK) ESP_LOGW(TAG, "async complete: %s", esp_err_to_name(complete));
        } else if (kind == JOB_ALBUM_SHOW) {
            err = screen_album_show(s_photo_pending);
            report.completed = err == ESP_OK;
            report.display_generation = screen_driver_generation();
            finish_job(err, &report);
        } else {
            screen_rotation_note_external();
            err = screen_driver_clear(&report);
            if (err == ESP_OK) screen_rotation_note_blank();
            finish_job(err, &report);
        }
        screen_rotation_unlock();
        ESP_LOGI(TAG, "job %s", err == ESP_OK ? "idle" : "error");
    }
}
static esp_err_t job_get_handler(httpd_req_t *req)
{
    return send_job_json(req);
}
static bool hex_header(httpd_req_t *req, const char *key, uint32_t *out)
{
    char value[9];
    if (httpd_req_get_hdr_value_str(req, key, value, sizeof(value)) != ESP_OK ||
        strlen(value) != 8 || strspn(value, "0123456789abcdefABCDEF") != 8) return false;
    *out = (uint32_t)strtoul(value, NULL, 16);
    return true;
}

static bool decimal_header(httpd_req_t *req, const char *key, uint32_t *out)
{
    char text[11];
    if (httpd_req_get_hdr_value_str(req, key, text, sizeof(text)) != ESP_OK || !text[0] ||
        strspn(text, "0123456789") != strlen(text)) return false;
    uint64_t value = strtoull(text, NULL, 10);
    if (value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

static esp_err_t album_read_request(void *context, uint8_t *buffer, size_t length)
{
    frame_rx_t *rx = context;
    if (xTaskGetTickCount() - rx->started > pdMS_TO_TICKS(90000)) return ESP_ERR_TIMEOUT;
    size_t used = 0;
    while (used < length) {
        const int count = httpd_req_recv(rx->req, (char *)buffer + used, length - used);
        if (count <= 0) return ESP_FAIL;
        used += count;
    }
    if (rx->bytes < SCREEN_FRAME_BYTES) for (size_t i = 0; i < length; ++i) {
        const unsigned a = buffer[i] >> 4, b = buffer[i] & 15;
        if (!((a <= 3 || a == 5 || a == 6) && (b <= 3 || b == 5 || b == 6)))
            return ESP_ERR_INVALID_ARG;
    }
    rx->bytes += length;
    return ESP_OK;
}

#define ALBUM_PAGE_SIZE 12
static esp_err_t photos_get(httpd_req_t *req)
{
    char query[32], value[12];
    size_t offset = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "offset", value, sizeof(value)) == ESP_OK) {
        char *end;
        unsigned long parsed = strtoul(value, &end, 10);
        if (!value[0] || *end || parsed > 100000)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid offset");
        offset = parsed;
    }
    char names[ALBUM_PAGE_SIZE + 1][13];
    const size_t count = tf_card_is_mounted()
        ? screen_album_list(offset, names, ALBUM_PAGE_SIZE + 1) : 0;
    char json[300];
    size_t used = snprintf(json, sizeof(json), "{\"mounted\":%s,\"more\":%s,\"photos\":[",
        tf_card_is_mounted() ? "true" : "false", count > ALBUM_PAGE_SIZE ? "true" : "false");
    for (size_t i = 0; i < count && i < ALBUM_PAGE_SIZE; ++i)
        used += snprintf(json + used, sizeof(json) - used, "%s\"%s\"", i ? "," : "", names[i]);
    memcpy(json + used, "]}", 3);
    return send_json(req, json);
}

static esp_err_t photos_delete(httpd_req_t *req)
{
    char body[512];
    if (read_json_body(req, body, sizeof(body)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid photo list");

    cJSON *items = cJSON_ParseWithOpts(body, NULL, true);
    const int count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    const char *names[24];
    bool removed[24];
    if (count < 1 || count > 24) {
        cJSON_Delete(items);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid photo count");
    }
    for (int i = 0; i < count; ++i) {
        const cJSON *name = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsString(name) || !screen_album_id_valid(name->valuestring)) {
            cJSON_Delete(items);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid photo ID");
        }
        names[i] = name->valuestring;
    }
    if (!tf_card_is_mounted()) {
        cJSON_Delete(items);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SD card not mounted");
    }

    const size_t deleted = screen_album_remove(names, count, removed);
    char json[450];
    size_t used = snprintf(json, sizeof(json), "{\"ok\":true,\"deleted\":%u,\"failed\":[", (unsigned)deleted);
    bool first = true;
    for (int i = 0; i < count; ++i) {
        if (removed[i]) continue;
        used += snprintf(json + used, sizeof(json) - used, "%s\"%s\"", first ? "" : ",", names[i]);
        first = false;
    }
    memcpy(json + used, "]}", 3);
    cJSON_Delete(items);
    return send_json(req, json);
}

static esp_err_t photo_upload_post(httpd_req_t *req)
{
    if (!tf_card_is_mounted()) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"message\":\"设备未挂载 SD 卡\"}");
    }
    frame_rx_t rx = {.req = req, .started = xTaskGetTickCount()};
    uint32_t original_size = 0, rotation = 0;
    if ((httpd_req_get_hdr_value_len(req, "X-Eink-Original-Size") &&
            !decimal_header(req, "X-Eink-Original-Size", &original_size)) ||
        (httpd_req_get_hdr_value_len(req, "X-Eink-Photo-Rotation") &&
            !decimal_header(req, "X-Eink-Photo-Rotation", &rotation)) ||
        rotation >= 360 || rotation % 90 ||
        original_size > 64U * 1024U * 1024U ||
        req->content_len != SCREEN_FRAME_BYTES + original_size ||
        !hex_header(req, "X-Eink-Checksum", &rx.expected))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid photo frame or original");
    if (web_server_screen_busy() || screen_rotation_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"message\":\"屏幕正忙，请稍后上传\"}");
    }
    char saved[13];
    const esp_err_t err = screen_album_upload(album_read_request, &rx, rx.expected, original_size, rotation, saved);
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    char json[48];
    snprintf(json, sizeof(json), "{\"ok\":true,\"id\":\"%s\"}", saved);
    return send_json(req, json);
}

typedef struct {
    httpd_req_t *req;
    bool started;
    char rotation[8];
} photo_sender_t;

static esp_err_t send_photo_chunk(void *opaque, unsigned rotation, const uint8_t *data, size_t length)
{
    photo_sender_t *sender = opaque;
    if (!sender->started) {
        snprintf(sender->rotation, sizeof(sender->rotation), "%u", rotation);
        httpd_resp_set_type(sender->req, "application/octet-stream");
        httpd_resp_set_hdr(sender->req, "X-Eink-Photo-Rotation", sender->rotation);
        httpd_resp_set_hdr(sender->req, "Cache-Control", "no-store");
        sender->started = true;
    }
    return httpd_resp_send_chunk(sender->req, (const char *)data, length);
}

static esp_err_t photo_preview_get(httpd_req_t *req)
{
    char query[40], name[13];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", name, sizeof(name)) != ESP_OK ||
        !screen_album_id_valid(name))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid photo");
    char full[3], original_flag[3];
    const bool original = httpd_query_key_value(query, "original", original_flag, sizeof(original_flag)) == ESP_OK &&
        strcmp(original_flag, "1") == 0;
    if (original || (httpd_query_key_value(query, "full", full, sizeof(full)) == ESP_OK && !strcmp(full, "1"))) {
        photo_sender_t sender = {.req = req};
        const esp_err_t err = screen_album_read(name, send_photo_chunk, &sender, original);
        if (err == ESP_OK) return httpd_resp_send_chunk(req, NULL, 0);
        if (sender.started) return ESP_FAIL;
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "photo unavailable");
    }
    uint8_t pixels[48 * 72];
    if (screen_album_thumbnail(name, pixels) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "photo unavailable");
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=3600");
    return httpd_resp_send(req, (const char *)pixels, sizeof(pixels));
}

static esp_err_t photo_show_post(httpd_req_t *req)
{
    char name[13];
    if (read_json_body(req, name, sizeof(name)) != ESP_OK || !screen_album_id_valid(name))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid photo");
    if (!begin_job(JOB_ALBUM_SHOW)) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"message\":\"屏幕正忙，请稍后重试\"}");
    }
    memcpy(s_photo_pending, name, sizeof(name));
    char json[48];
    snprintf(json, sizeof(json), "{\"ok\":true,\"id\":%u}", (unsigned)s_job_id);
    xTaskNotifyGive(s_job_task);
    httpd_resp_set_status(req, "202 Accepted");
    return send_json(req, json);
}

static esp_err_t frame_post_handler(httpd_req_t *req)
{
    char format[16], pass[8], quality[12];
    frame_rx_t rx = { .hash = 2166136261U, .mode = SCREEN_MODE_PHOTO };
    char mode[12];
    if (httpd_req_get_hdr_value_len(req, "X-Eink-Mode")) {
        if (httpd_req_get_hdr_value_str(req, "X-Eink-Mode", mode, sizeof(mode)) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid display mode");
        }
        if (strcmp(mode, "text") == 0) rx.mode = SCREEN_MODE_TEXT;
        else if (strcmp(mode, "photo") != 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid display mode");
        }
    }
    if (httpd_req_get_hdr_value_str(req, "X-Eink-Format", format, sizeof(format)) != ESP_OK ||
        strcmp(format, FRAME_FORMAT) != 0 ||
        httpd_req_get_hdr_value_str(req, "X-Eink-Pass", pass, sizeof(pass)) != ESP_OK ||
        strcmp(pass, "0") != 0 ||
        httpd_req_get_hdr_value_str(req, "X-Eink-Quality", quality, sizeof(quality)) != ESP_OK ||
        (strcmp(quality, "fast") != 0 && strcmp(quality, "standard") != 0 && strcmp(quality, "best") != 0) ||
        !hex_header(req, "X-Eink-Checksum", &rx.expected)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "frame contract mismatch; reload page");
        return ESP_FAIL;
    }
    char storage[12];
    rx.save_to_album = httpd_req_get_hdr_value_str(req, "X-Eink-Storage", storage, sizeof(storage)) != ESP_OK ||
        strcmp(storage, "display") != 0;
    if (rx.mode == SCREEN_MODE_PHOTO && rx.save_to_album && !tf_card_is_mounted()) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"message\":\"请插入 TF 卡并重启设备后再上传照片\"}");
    }
    if (req->content_len != SCREEN_FRAME_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected 172800 bytes");
        return ESP_FAIL;
    }
    if (httpd_req_get_hdr_value_len(req, "X-Eink-Base-Generation") &&
        (!decimal_header(req, "X-Eink-Base-Generation", &rx.base_generation) ||
         !rx.base_generation || rx.base_generation != screen_driver_generation())) {
        httpd_resp_set_status(req, "409 Conflict");
        send_json(req, "{\"ok\":false,\"error\":\"region baseline changed; send a full image\"}");
        return ESP_FAIL;
    }
    if (!decimal_header(req, "X-Eink-Rotation-Epoch", &rx.rotation_epoch) ||
        !screen_rotation_matches(rx.rotation_epoch) || !begin_job(JOB_FRAME)) {
        httpd_resp_set_status(req, "409 Conflict");
        send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
        return ESP_FAIL;
    }
    esp_err_t err = httpd_req_async_handler_begin(req, &rx.req);
    if (err != ESP_OK) {
        const screen_upload_report_t empty = {0};
        finish_job(err, &empty);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    rx.id = s_job_id;
    s_pending = rx;
    ESP_LOGI(TAG, "frame id=%u format=%s quality=%s bytes=%u",
             (unsigned)rx.id, format, quality, (unsigned)req->content_len);
    log_heap_stats("frame-start", rx.id);
    /* The worker owns first byte through POF and response; HTTP stays available. */
    xTaskNotifyGive(s_job_task);
    return ESP_OK;
}

static esp_err_t panel_get_handler(httpd_req_t *req)
{
    return send_json(req, "{\"ok\":true,\"controller\":\"EPD037A02\","
        "\"image_formats\":[\"otp6-v1\"],\"colors\":6,\"full_refresh\":true}");
}

static esp_err_t clear_post_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected empty body");
        return ESP_FAIL;
    }
    if (!begin_job(JOB_CLEAR_SCREEN)) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
    }
    portENTER_CRITICAL(&s_job_lock);
    s_job_phase = JOB_REFRESH;
    s_job_step = "clear";
    portEXIT_CRITICAL(&s_job_lock);
    xTaskNotifyGive(s_job_task);
    httpd_resp_set_status(req, "202 Accepted");
    return send_job_json(req);
}

static esp_err_t management_request(httpd_req_t *req)
{
    const httpd_uri_t *route = req->user_ctx;
    if (!wifi_mgr_management_active()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_json(req, "{\"ok\":false}");
    }
    const bool user_activity = strcmp(route->uri, "/api/status") && strcmp(route->uri, "/api/job");
    if (user_activity) {
        __atomic_store_n(&s_user_request_active, true, __ATOMIC_RELEASE);
        web_server_note_activity();
    }
    const esp_err_t err = route->handler(req);
    if (user_activity) {
        web_server_note_activity();
        __atomic_store_n(&s_user_request_active, false, __ATOMIC_RELEASE);
    }
    return err;
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 43;
    config.stack_size = 12288;
    config.max_open_sockets = 7;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 8;
    config.lru_purge_enable = true;

    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);

    if (s_job_task == NULL &&
        xTaskCreate(frame_job_task, "eink_job", 4096, NULL, 5, &s_job_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start");

    static const httpd_uri_t handlers[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/vue.global.prod.js", .method = HTTP_GET, .handler = vue_get_handler },
        { .uri = "/eink_pipeline.js", .method = HTTP_GET, .handler = pipeline_get_handler },
        { .uri = "/weather.js", .method = HTTP_GET, .handler = weather_script_get },
        { .uri = "/api/weather/location", .method = HTTP_GET, .handler = weather_settings_get },
        { .uri = "/api/weather/location", .method = HTTP_POST, .handler = weather_settings_post },
        { .uri = "/api/weather/policy", .method = HTTP_GET, .handler = weather_policy },
        { .uri = "/api/weather/policy", .method = HTTP_POST, .handler = weather_policy },
        { .uri = "/api/display/schedule", .method = HTTP_GET, .handler = schedule_get },
        { .uri = "/api/display/schedule", .method = HTTP_POST, .handler = schedule_post },
        { .uri = "/api/display/style", .method = HTTP_GET, .handler = display_style },
        { .uri = "/api/display/style", .method = HTTP_POST, .handler = display_style },
        { .uri = "/api/trips", .method = HTTP_GET, .handler = trips_handler },
        { .uri = "/api/trips", .method = HTTP_POST, .handler = trips_handler },
        { .uri = "/api/trips/select", .method = HTTP_POST, .handler = trip_select_post },
        { .uri = "/place-catalog.json", .method = HTTP_GET, .handler = place_catalog_get },
        { .uri = "/api/history", .method = HTTP_GET, .handler = history_get },
        { .uri = "/api/lunar", .method = HTTP_POST, .handler = lunar_post },
        { .uri = "/lunar.js", .method = HTTP_GET, .handler = lunar_script },
        { .uri = "/api/display/shortcut", .method = HTTP_GET, .handler = shortcut_handler },
        { .uri = "/api/display/shortcut", .method = HTTP_POST, .handler = shortcut_handler },
        { .uri = "/api/display/refresh", .method = HTTP_POST, .handler = refresh_mode_post },
        { .uri = "/api/todo", .method = HTTP_GET, .handler = todo_get },
        { .uri = "/api/todo", .method = HTTP_POST, .handler = todo_post },
        { .uri = "/api/quotes", .method = HTTP_GET, .handler = quotes_get },
        { .uri = "/api/quotes", .method = HTTP_POST, .handler = quotes_post },
        { .uri = "/api/quotes/show", .method = HTTP_POST, .handler = quote_show_post },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler },
        { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler },
        { .uri = "/api/wifi/off", .method = HTTP_POST, .handler = wifi_off_post },
        { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_handler },
        { .uri = "/api/charge", .method = HTTP_GET, .handler = charge_get_handler },
        { .uri = "/api/charge", .method = HTTP_POST, .handler = charge_post_handler },
        { .uri = "/api/photos", .method = HTTP_GET, .handler = photos_get },
        { .uri = "/api/photos", .method = HTTP_POST, .handler = photo_upload_post },
        { .uri = "/api/photos", .method = HTTP_DELETE, .handler = photos_delete },
        { .uri = "/api/photos/preview", .method = HTTP_GET, .handler = photo_preview_get },
        { .uri = "/api/photos/show", .method = HTTP_POST, .handler = photo_show_post },
        { .uri = "/api/frame", .method = HTTP_POST, .handler = frame_post_handler },
        { .uri = "/api/job", .method = HTTP_GET, .handler = job_get_handler },
        { .uri = "/api/panel", .method = HTTP_GET, .handler = panel_get_handler },
        { .uri = "/api/clear", .method = HTTP_POST, .handler = clear_post_handler },
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        httpd_uri_t route = handlers[i];
        route.handler = management_request;
        route.user_ctx = (void *)&handlers[i];
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &route), TAG, "register URI");
    }

    ESP_LOGI(TAG, "HTTP ready on :80");
    return ESP_OK;
}
