#include "dashboard.h"
#include "page_data.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "pcf8563.h"
#include "tf_card.h"
#include "wifi_mgr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>

#define SETTINGS_NS "eink_dashboard"
#define FORECAST_BYTES 8192
#define NMC_HOST "https://www.nmc.cn"
#define NMC_PATH_PREFIX "/publish/forecast/"
#define NMC_PATH_BYTES 96
#define NMC_DEFAULT_PATH "/publish/forecast/AJS/yangzhou.html"
#define EARLIEST_TIME 1735689600LL

static uint8_t s_weather_name[WEATHER_NAME_BYTES];
static weather_data_t s_weather;
static bool s_default_name;
static bool s_weather_ready;
static bool s_weather_live;
static bool s_weather_offline;
static uint32_t s_weather_attempt_day;
static uint32_t s_clock_synced_at;
static uint32_t s_location_revision;
static const char *TAG = "dashboard";

typedef struct {
    uint32_t location;
    weather_data_t weather;
    uint32_t fetched_at;
} weather_cache_t;

#define WEATHER_SD_FILE TF_CARD_MOUNT_POINT "/WEATHER.BIN"
#define WEATHER_SD_TEMP TF_CARD_MOUNT_POINT "/WEATHER.TMP"
#define WEATHER_SD_BACKUP TF_CARD_MOUNT_POINT "/WEATHER.BAK"
#define WEATHER_SD_MAGIC UINT32_C(0x32544857)

typedef struct {
    uint32_t magic;
    uint32_t crc;
    weather_cache_t cache;
} weather_sd_record_t;

static uint32_t weather_cache_crc(const weather_cache_t *cache)
{
    return esp_rom_crc32_le(0, (const uint8_t *)cache, sizeof(*cache));
}

static bool weather_sd_read_file(const char *path, weather_cache_t *cache)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    weather_sd_record_t record;
    const bool valid = fread(&record, sizeof(record), 1, file) == 1 && fgetc(file) == EOF &&
        !ferror(file) && record.magic == WEATHER_SD_MAGIC &&
        record.crc == weather_cache_crc(&record.cache);
    fclose(file);
    if (valid) *cache = record.cache;
    return valid;
}

static esp_err_t weather_sd_store(const weather_cache_t *cache)
{
    if (!tf_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    weather_sd_record_t record = {.magic = WEATHER_SD_MAGIC};
    memcpy(&record.cache, cache, sizeof(*cache));
    record.crc = weather_cache_crc(&record.cache);
    FILE *file = fopen(WEATHER_SD_TEMP, "wb");
    if (!file) return ESP_FAIL;
    bool ok = fwrite(&record, sizeof(record), 1, file) == 1 &&
        fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    struct stat info;
    if (ok && stat(WEATHER_SD_FILE, &info) == 0) {
        remove(WEATHER_SD_BACKUP);
        ok = rename(WEATHER_SD_FILE, WEATHER_SD_BACKUP) == 0;
    }
    if (ok && rename(WEATHER_SD_TEMP, WEATHER_SD_FILE) == 0) {
        remove(WEATHER_SD_BACKUP);
        return ESP_OK;
    }
    if (stat(WEATHER_SD_FILE, &info) != 0) rename(WEATHER_SD_BACKUP, WEATHER_SD_FILE);
    remove(WEATHER_SD_TEMP);
    return ESP_FAIL;
}

const char *dashboard_default_location(void)
{
    return "{\"name\":\"扬州\",\"latitude\":32.41,\"longitude\":119.42,"
        "\"stationPath\":\"" NMC_DEFAULT_PATH "\",\"isDefault\":true}";
}

bool dashboard_weather_ready(void)
{
    return __atomic_load_n(&s_weather_ready, __ATOMIC_ACQUIRE);
}

bool dashboard_weather_is_live(void)
{
    return __atomic_load_n(&s_weather_live, __ATOMIC_ACQUIRE);
}

static uint32_t location_hash(const char *body)
{
    uint32_t hash = 2166136261u;
    while (*body) {
        hash = (hash ^ (uint8_t)*body++) * 16777619u;
    }
    return hash;
}

static esp_err_t read_setting(const char *key, void *data, size_t *size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(handle, key, data, size);
    nvs_close(handle);
    return err;
}

static esp_err_t write_setting(const char *key, const void *data, size_t size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, key, data, size);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static uint8_t s_mono_mask;

uint8_t dashboard_mono_mask(void)
{
    return __atomic_load_n(&s_mono_mask, __ATOMIC_ACQUIRE);
}

void dashboard_display_style_restore(void)
{
    uint8_t mask = 0;
    size_t size = sizeof(mask);
    if (read_setting("mono_pages", &mask, &size) == ESP_OK &&
            size == sizeof(mask) && (mask & ~((1U << SCREEN_MODE_COUNT) - 1)) == 0) {
        __atomic_store_n(&s_mono_mask, mask, __ATOMIC_RELEASE);
    }
}

esp_err_t dashboard_mono_set(uint8_t mask)
{
    if (mask & ~((1U << SCREEN_MODE_COUNT) - 1)) return ESP_ERR_INVALID_ARG;
    const esp_err_t err = write_setting("mono_pages", &mask, sizeof(mask));
    if (err == ESP_OK) __atomic_store_n(&s_mono_mask, mask, __ATOMIC_RELEASE);
    return err;
}

static bool schedule_valid(const dashboard_schedule_t *schedule)
{
    return schedule->enabled && (schedule->enabled & ~((1U << SCREEN_MODE_COUNT) - 1)) == 0 &&
        schedule->every >= 1 && schedule->every <= 1439;
}

esp_err_t dashboard_todo_get(char *body, size_t size)
{
    const esp_err_t sd = page_sd_read("TODO.JSN", body, size);
    if (sd == ESP_OK) return ESP_OK;
    if (sd != ESP_ERR_NOT_FOUND && sd != ESP_ERR_INVALID_STATE) return sd;
    const esp_err_t err = read_setting("todo_v1", body, &size);
    if (err == ESP_OK && (!size || body[size - 1] != '\0')) return ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && sd == ESP_ERR_NOT_FOUND) (void)page_sd_write("TODO.JSN", body);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

static bool parse_todo(const char *body, uint8_t *bitmap, unsigned *count, unsigned *done, unsigned *zoom,
    char due[TODO_MAX_ITEMS][TODO_DUE_BYTES])
{
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    const cJSON *image = cJSON_GetObjectItemCaseSensitive(root, "bitmap");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "zoom");
    const double scale = size ? size->valuedouble : 100;
    const int length = cJSON_GetArraySize(items);
    unsigned mask = 0;
    bool valid = cJSON_IsArray(items) && length <= TODO_MAX_ITEMS && cJSON_IsString(image) &&
        (!size || cJSON_IsNumber(size)) && (scale == 100 || scale == 125 || scale == 150);
    for (int i = 0; valid && i < length; ++i) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(item, "text");
        const cJSON *checked = cJSON_GetObjectItemCaseSensitive(item, "done");
        const cJSON *deadline = cJSON_GetObjectItemCaseSensitive(item, "due");
        valid = cJSON_IsString(label) && label->valuestring[0] &&
            strlen(label->valuestring) <= 88 && !strpbrk(label->valuestring, "\r\n") &&
            cJSON_IsBool(checked);
        if (deadline) {
            valid = valid && cJSON_IsString(deadline) && page_deadline_valid(deadline->valuestring);
            if (valid && due) strcpy(due[i], deadline->valuestring);
        }
        if (cJSON_IsTrue(checked)) mask |= 1U << i;
    }
    if (valid) {
        size_t decoded = 0;
        const size_t expected = length * TODO_ROW_BYTES;
        valid = mbedtls_base64_decode(bitmap, TODO_MAX_ITEMS * TODO_ROW_BYTES, &decoded,
            (const unsigned char *)image->valuestring, strlen(image->valuestring)) == 0 &&
            decoded == expected;
    }
    cJSON_Delete(root);
    if (valid) {
        *count = length;
        *done = mask;
        *zoom = (unsigned)scale;
    }
    return valid;
}

esp_err_t dashboard_todo_set(const char *body)
{
    uint8_t *bitmap = malloc(TODO_MAX_ITEMS * TODO_ROW_BYTES);
    if (!bitmap) return ESP_ERR_NO_MEM;
    unsigned count, done, zoom;
    const bool valid = parse_todo(body, bitmap, &count, &done, &zoom, NULL);
    free(bitmap);
    if (!valid) return ESP_ERR_INVALID_ARG;
    return page_sd_write("TODO.JSN", body);
}

esp_err_t dashboard_todo_render_data(uint8_t *bitmap, unsigned *count, unsigned *done, unsigned *zoom,
    char due[TODO_MAX_ITEMS][TODO_DUE_BYTES])
{
    *count = *done = 0;
    *zoom = 100;
    char *body = malloc(TODO_JSON_BYTES);
    if (!body) return ESP_ERR_NO_MEM;
    esp_err_t err = dashboard_todo_get(body, TODO_JSON_BYTES);
    if (err == ESP_OK && !parse_todo(body, bitmap, count, done, zoom, due)) err = ESP_ERR_INVALID_ARG;
    free(body);
    return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
}

void dashboard_schedule_get(dashboard_schedule_t *schedule)
{
    size_t size = sizeof(*schedule);
    if (read_setting("carousel_v2", schedule, &size) == ESP_OK &&
        size == sizeof(*schedule) && schedule_valid(schedule)) return;
    *schedule = (dashboard_schedule_t){.enabled = (1U << SCREEN_MODE_TRIP) - 1,
        .running = false, .every = 5};
}

esp_err_t dashboard_schedule_set(const dashboard_schedule_t *schedule)
{
    if (!schedule_valid(schedule)) return ESP_ERR_INVALID_ARG;
    return write_setting("carousel_v2", schedule, sizeof(*schedule));
}

esp_err_t dashboard_location_get(char *body, size_t size)
{
    esp_err_t err = read_setting("location_v2", body, &size);
    if (err == ESP_OK && (size == 0 || body[size - 1] != '\0')) return ESP_ERR_INVALID_SIZE;
    return err;
}

static bool station_path_valid(const char *path)
{
    const size_t prefix = strlen(NMC_PATH_PREFIX);
    const size_t length = strlen(path);
    if (length <= prefix + 5 || length >= NMC_PATH_BYTES ||
        strncmp(path, NMC_PATH_PREFIX, prefix) || strcmp(path + length - 5, ".html")) return false;
    for (size_t i = prefix; i < length - 5; ++i) {
        const unsigned char c = path[i];
        if (!isalnum(c) && c != '/' && c != '_' && c != '-') return false;
    }
    return true;
}

static bool parse_location(const char *body, double *latitude, double *longitude, uint8_t *name_bitmap, char *station)
{
    cJSON *json = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(json, "latitude");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(json, "longitude");
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(json, "stationPath");
    const cJSON *bitmap = cJSON_GetObjectItemCaseSensitive(json, "bitmap");
    size_t decoded = 0;
    const bool valid = cJSON_IsString(name) && name->valuestring[0] &&
        strlen(name->valuestring) <= 120 &&
        cJSON_IsNumber(lat) && lat->valuedouble >= -90 && lat->valuedouble <= 90 &&
        cJSON_IsNumber(lon) && lon->valuedouble >= -180 && lon->valuedouble <= 180 &&
        (!path || (cJSON_IsString(path) && station_path_valid(path->valuestring))) &&
        cJSON_IsString(bitmap) &&
        mbedtls_base64_decode(name_bitmap, WEATHER_NAME_BYTES, &decoded,
            (const unsigned char *)bitmap->valuestring, strlen(bitmap->valuestring)) == 0 &&
        decoded == WEATHER_NAME_BYTES;
    if (valid) {
        *latitude = lat->valuedouble;
        *longitude = lon->valuedouble;
        snprintf(station, NMC_PATH_BYTES, "%s", path ? path->valuestring : "");
    }
    cJSON_Delete(json);
    return valid;
}

esp_err_t dashboard_location_set(const char *body)
{
    const size_t size = strlen(body) + 1;
    uint8_t bitmap[WEATHER_NAME_BYTES];
    double lat, lon;
    char station[NMC_PATH_BYTES];
    if (size > WEATHER_LOCATION_BYTES || !parse_location(body, &lat, &lon, bitmap, station)) return ESP_ERR_INVALID_ARG;
    screen_rotation_lock();
    const esp_err_t err = write_setting("location_v2", body, size);
    if (err == ESP_OK) {
        (void)__atomic_add_fetch(&s_location_revision, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&s_weather_ready, false, __ATOMIC_RELEASE);

        __atomic_store_n(&s_weather_live, false, __ATOMIC_RELEASE);
    }
    screen_rotation_unlock();
    return err;
}

static bool weather_nvs_restore(weather_cache_t *cached)
{
    size_t size = sizeof(*cached);
    if (read_setting("weather_v2", cached, &size) == ESP_OK && size == sizeof(*cached)) return true;

    const size_t legacy_size = offsetof(weather_cache_t, weather) +
        offsetof(weather_data_t, daily) + 6 * sizeof(cached->weather.daily[0]);
    unsigned char legacy[sizeof(*cached)];
    size = sizeof(legacy);
    if (read_setting("weather_v1", legacy, &size) != ESP_OK ||
            (size != legacy_size && size != legacy_size + sizeof(cached->fetched_at))) return false;
    memset(cached, 0, sizeof(*cached));
    memcpy(cached, legacy, legacy_size);
    if (size > legacy_size) memcpy(&cached->fetched_at, legacy + legacy_size, sizeof(cached->fetched_at));
    cached->weather.daily[6].code = WEATHER_MISSING;
    cached->weather.daily[6].low = WEATHER_MISSING;
    cached->weather.daily[6].high = WEATHER_MISSING;
    return true;
}

static bool weather_cache_valid(const weather_cache_t *cache, uint32_t location)
{
    const weather_data_t *weather = &cache->weather;
    return cache->location == location && weather->weekday >= 0 && weather->weekday <= 6 &&
        weather->year >= 2025 && weather->year < 2100 &&
        weather->month >= 1 && weather->month <= 12 && weather->day >= 1 && weather->day <= 31 &&
        weather->hour >= 0 && weather->hour < 24 && weather->minute >= 0 && weather->minute < 60;
}

void dashboard_weather_restore(void)
{
    uint8_t offline = 0;
    size_t offline_size = sizeof(offline);
    if (read_setting("weather_offline", &offline, &offline_size) == ESP_OK)
        __atomic_store_n(&s_weather_offline, offline == 1, __ATOMIC_RELEASE);
    size_t attempt_size = sizeof(s_weather_attempt_day);
    (void)read_setting("weather_attempt", &s_weather_attempt_day, &attempt_size);
    char body[WEATHER_LOCATION_BYTES];
    const esp_err_t err = dashboard_location_get(body, sizeof(body));
    const bool use_default = err == ESP_ERR_NVS_NOT_FOUND;
    if (err != ESP_OK && !use_default) return;
    const uint32_t location = location_hash(use_default ? dashboard_default_location() : body);
    weather_cache_t cached = {0};
    const bool mounted = tf_card_is_mounted();
    const bool sd_valid = mounted &&
        ((weather_sd_read_file(WEATHER_SD_FILE, &cached) && weather_cache_valid(&cached, location)) ||
         (weather_sd_read_file(WEATHER_SD_BACKUP, &cached) && weather_cache_valid(&cached, location)));
    weather_cache_t fallback = {0};
    const bool nvs_valid = weather_nvs_restore(&fallback) && weather_cache_valid(&fallback, location);
    bool loaded_from_sd = sd_valid;
    if (nvs_valid && (!sd_valid || fallback.fetched_at > cached.fetched_at)) {
        cached = fallback;
        loaded_from_sd = false;
        if (mounted && weather_sd_store(&cached) == ESP_OK)
            ESP_LOGI(TAG, "Migrated weather cache to " WEATHER_SD_FILE);
    } else if (!sd_valid) return;
    double lat, lon;
    char station[NMC_PATH_BYTES];
    if (!use_default && !parse_location(body, &lat, &lon, s_weather_name, station)) return;

    s_weather = cached.weather;
    struct tm observed = {
        .tm_year = s_weather.year - 1900, .tm_mon = s_weather.month - 1,
        .tm_mday = s_weather.day, .tm_hour = s_weather.hour, .tm_min = s_weather.minute,
    };
    const time_t last_fetch = cached.fetched_at ? cached.fetched_at : mktime(&observed);
    struct tm last;
    if (!s_weather_attempt_day && last_fetch >= EARLIEST_TIME && localtime_r(&last_fetch, &last)) {
        s_weather_attempt_day = (last.tm_year + 1900) * 10000 + (last.tm_mon + 1) * 100 + last.tm_mday;
    }
    s_default_name = use_default;
    __atomic_store_n(&s_weather_ready, true, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "restored weather cache from %s", loaded_from_sd ? "SD card" : "internal storage");
}

static void sync_rtc(struct timeval *value)
{
    struct tm utc;
    gmtime_r(&value->tv_sec, &utc);
    const pcf8563_time_t rtc = {
        .year = utc.tm_year + 1900, .month = utc.tm_mon + 1, .day = utc.tm_mday,
        .weekday = utc.tm_wday, .hour = utc.tm_hour, .minute = utc.tm_min, .second = utc.tm_sec,
    };
    (void)pcf8563_set_time(&rtc);
    __atomic_store_n(&s_clock_synced_at, (uint32_t)value->tv_sec, __ATOMIC_RELEASE);
}

void dashboard_clock_start(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    pcf8563_time_t rtc;
    if (pcf8563_get_time(&rtc) == ESP_OK && rtc.year >= 2025 && rtc.year < 2100 &&
        rtc.month >= 1 && rtc.month <= 12 && rtc.day >= 1 && rtc.day <= 31 &&
        rtc.hour < 24 && rtc.minute < 60 && rtc.second < 60) {
        /* The hardware clock stores UTC. The display and schedule use UTC+8. */
        struct tm raw = {
            .tm_year = rtc.year - 1900, .tm_mon = rtc.month - 1, .tm_mday = rtc.day,
            .tm_hour = rtc.hour, .tm_min = rtc.minute, .tm_sec = rtc.second,
        };
        struct timeval time = { .tv_sec = mktime(&raw) + 8 * 60 * 60 };
        settimeofday(&time, NULL);
    }
}

bool dashboard_clock_sync_start(void)
{
    const time_t now = time(NULL);
    const time_t synced = __atomic_load_n(&s_clock_synced_at, __ATOMIC_ACQUIRE);
    if (synced && now >= synced && now - synced < 86400) return false;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.sync_cb = sync_rtc;
    return esp_netif_sntp_init(&config) == ESP_OK;
}

void dashboard_clock_sync_stop(void)
{
    esp_netif_sntp_deinit();
}

bool dashboard_weather_claim_today(void)
{
    struct tm local;
    if (!dashboard_local_time(&local)) return false;
    const uint32_t today = (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
    if (s_weather_attempt_day == today) return false;
    s_weather_attempt_day = today;
    const esp_err_t err = write_setting("weather_attempt", &today, sizeof(today));
    if (err != ESP_OK) ESP_LOGW(TAG, "Weather attempt date save failed: %s", esp_err_to_name(err));
    return true;
}

bool dashboard_weather_offline(void)
{
    return __atomic_load_n(&s_weather_offline, __ATOMIC_ACQUIRE);
}

esp_err_t dashboard_weather_set_offline(bool offline)
{
    const uint8_t value = offline;
    const esp_err_t err = write_setting("weather_offline", &value, sizeof(value));
    if (err == ESP_OK) __atomic_store_n(&s_weather_offline, offline, __ATOMIC_RELEASE);
    return err;
}

void dashboard_weather_use_cache(void)
{
    __atomic_store_n(&s_weather_live, false, __ATOMIC_RELEASE);
}

bool dashboard_local_time(struct tm *now)
{
    const time_t epoch = time(NULL);
    return epoch >= EARLIEST_TIME && localtime_r(&epoch, now) != NULL;
}

typedef struct {
    char *body;
    size_t used;
    int64_t deadline;
} response_t;

static esp_err_t receive_json(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    response_t *response = event->user_data;
    if (esp_timer_get_time() >= response->deadline) return ESP_ERR_TIMEOUT;
    if (event->data_len < 0 || (size_t)event->data_len >= FORECAST_BYTES - response->used) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(response->body + response->used, event->data, event->data_len);
    response->used += event->data_len;
    response->body[response->used] = 0;
    return ESP_OK;
}

static int request_timeout(int64_t deadline)
{
    const int64_t remaining = (deadline - esp_timer_get_time()) / 1000;
    return remaining < 4000 ? (int)remaining : 4000;
}

static cJSON *get_json(const char *url, char *buffer, int64_t deadline)
{
    const int timeout = request_timeout(deadline);
    if (timeout < 500 || !wifi_mgr_is_sta_up()) return NULL;
    response_t response = { .body = buffer, .deadline = deadline };
    buffer[0] = 0;
    const esp_http_client_config_t config = {
        .url = url, .timeout_ms = timeout, .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = receive_json, .user_data = &response, .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK && status == 200 ? cJSON_Parse(buffer) : NULL;
}

static int number(const cJSON *item)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < -999 || item->valuedouble > 999) return WEATHER_MISSING;
    return (int)(item->valuedouble + (item->valuedouble < 0 ? -0.5 : 0.5));
}

static int entry(const cJSON *object, const char *key)
{
    return number(cJSON_GetObjectItemCaseSensitive(object, key));
}

static int daily_entry(const cJSON *object, const char *key, int i)
{
    return number(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(object, key), i));
}

static bool read_timestamp(const char *timestamp, weather_data_t *next)
{
    char delimiter;
    if (!timestamp || strlen(timestamp) < 16 ||
        sscanf(timestamp, "%d-%d-%d%c%d:%d", &next->year, &next->month, &next->day,
            &delimiter, &next->hour, &next->minute) != 6 ||
        (delimiter != ' ' && delimiter != 'T') || next->year < 2025 || next->year > 2099 ||
        next->month < 1 || next->month > 12 || next->day < 1 || next->day > 31 ||
        next->hour < 0 || next->hour > 23 || next->minute < 0 || next->minute > 59) return false;
    struct tm date = { .tm_year = next->year - 1900, .tm_mon = next->month - 1, .tm_mday = next->day };
    (void)mktime(&date);
    next->weekday = date.tm_wday;
    return true;
}

typedef struct {
    unsigned matched, count;
    int64_t deadline;
    bool collecting, finished;
    char code[24];
} station_code_t;

static esp_err_t scan_station_code(esp_http_client_event_t *event)
{
    static const char marker[] = "var scode = '";
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    station_code_t *state = event->user_data;
    if (esp_timer_get_time() >= state->deadline) return ESP_ERR_TIMEOUT;
    const unsigned char *data = event->data;
    for (int i = 0; i < event->data_len && !state->finished; ++i) {
        unsigned char c = data[i];
        if (state->collecting) {
            if (c == '\'' || !isalnum(c) || state->count == sizeof(state->code) - 1) {
                state->finished = true;
                if (c != '\'') state->code[0] = 0;
            } else {
                state->code[state->count++] = c;
            }
        } else {
            state->matched = c == marker[state->matched] ? state->matched + 1 : c == marker[0] ? 1 : 0;
            state->collecting = state->matched == sizeof(marker) - 1;
        }
    }
    return ESP_OK;
}

static bool nmc_station_code(const char *path, station_code_t *state)
{
    const int timeout = request_timeout(state->deadline);
    if (timeout < 500 || !wifi_mgr_is_sta_up()) return false;
    char url[sizeof(NMC_HOST) + NMC_PATH_BYTES];
    snprintf(url, sizeof(url), NMC_HOST "%s", path);
    const esp_http_client_config_t config = {
        .url = url, .timeout_ms = timeout, .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = scan_station_code, .user_data = state, .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK && status == 200 && state->finished && state->code[0];
}

static int number_text(const cJSON *item)
{
    if (!cJSON_IsString(item)) return number(item);
    const char *start = item->valuestring;
    char *end;
    long n = strtol(start, &end, 10);
    return start != end && *end == 0 && n >= -999 && n <= 999 ? (int)n : WEATHER_MISSING;
}

static int nmc_condition(const cJSON *weather)
{
    const int code = number_text(cJSON_GetObjectItemCaseSensitive(weather, "img"));
    if (code == 0) return 0;
    if (code == 1) return 2;
    if (code == 2) return 3;
    if (code == 4 || code == 5) return 95;
    if (code == 18 || code == 32 || code == 53) return 45;
    if ((code >= 13 && code <= 17) || (code >= 26 && code <= 28)) return 73;
    if ((code >= 3 && code <= 12) || code == 19 || (code >= 21 && code <= 25)) return 61;
    return WEATHER_MISSING;
}

static esp_err_t load_nmc(const char *path, char *buffer, weather_data_t *next, int64_t deadline)
{
    station_code_t station = {.deadline = deadline};
    if (!nmc_station_code(path, &station)) return ESP_FAIL;
    char url[112];
    snprintf(url, sizeof(url), NMC_HOST "/rest/real/%s", station.code);
    cJSON *json = get_json(url, buffer, deadline);
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(json, "weather");
    const cJSON *wind = cJSON_GetObjectItemCaseSensitive(json, "wind");
    const cJSON *stamp = cJSON_GetObjectItemCaseSensitive(json, "publish_time");
    *next = (weather_data_t){
        .nmc = true, .temperature = entry(current, "temperature"), .humidity = entry(current, "humidity"),
        .code = nmc_condition(current), .wind = entry(wind, "speed"), .direction = entry(wind, "degree"),
        .aqi = WEATHER_MISSING,
    };
    const bool valid = next->temperature != WEATHER_MISSING && cJSON_IsString(stamp) &&
        read_timestamp(stamp->valuestring, next);
    cJSON_Delete(json);
    if (!valid) return ESP_FAIL;

    snprintf(url, sizeof(url), NMC_HOST "/rest/predict/%s", station.code);
    json = get_json(url, buffer, deadline);
    const cJSON *days = cJSON_GetObjectItemCaseSensitive(json, "detail");
    if (!cJSON_IsArray(days) || !cJSON_GetArraySize(days)) {
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    struct tm date = { .tm_year = next->year - 1900, .tm_mon = next->month - 1, .tm_mday = next->day };
    for (int i = 0; i < WEATHER_DAYS; ++i) {
        (void)mktime(&date);
        char wanted[32];
        strftime(wanted, sizeof(wanted), "%Y-%m-%d", &date);
        const cJSON *item;
        const cJSON *day = NULL, *night = NULL;
        cJSON_ArrayForEach(item, days) {
            const cJSON *iso = cJSON_GetObjectItemCaseSensitive(item, "date");
            if (!cJSON_IsString(iso) || strcmp(iso->valuestring, wanted)) continue;
            day = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item, "day"), "weather");
            night = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item, "night"), "weather");
            break;
        }
        const int code = nmc_condition(day);
        next->daily[i].code = code != WEATHER_MISSING ? code : nmc_condition(night);
        next->daily[i].high = number_text(cJSON_GetObjectItemCaseSensitive(day, "temperature"));
        next->daily[i].low = number_text(cJSON_GetObjectItemCaseSensitive(night, "temperature"));
        ++date.tm_mday;
    }
    cJSON_Delete(json);
    if (next->code == WEATHER_MISSING) next->code = next->daily[0].code;

    if (request_timeout(deadline) < 1500) return ESP_OK;
    snprintf(url, sizeof(url), NMC_HOST "/rest/aqi/%s", station.code);
    json = get_json(url, buffer, deadline);
    next->aqi = entry(json, "aqi");
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t load_open_meteo(double lat, double lon, char *buffer, weather_data_t *result, int64_t deadline)
{
    char url[448];
    snprintf(url, sizeof(url), "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f"
        "&timezone=auto&forecast_days=7&wind_speed_unit=ms"
        "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min", lat, lon);
    cJSON *json = get_json(url, buffer, deadline);
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(json, "current");
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(json, "daily");
    weather_data_t next = {
        .temperature = entry(current, "temperature_2m"),
        .humidity = entry(current, "relative_humidity_2m"),
        .code = entry(current, "weather_code"),
        .wind = entry(current, "wind_speed_10m"),
        .direction = entry(current, "wind_direction_10m"),
        .aqi = WEATHER_MISSING,
    };
    const cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(current, "time");
    if (next.temperature == WEATHER_MISSING || !cJSON_IsString(timestamp) ||
        !read_timestamp(timestamp->valuestring, &next)) {
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    for (int i = 0; i < WEATHER_DAYS; ++i) {
        next.daily[i].code = daily_entry(daily, "weather_code", i);
        next.daily[i].low = daily_entry(daily, "temperature_2m_min", i);
        next.daily[i].high = daily_entry(daily, "temperature_2m_max", i);
    }
    cJSON_Delete(json);

    snprintf(url, sizeof(url), "https://air-quality-api.open-meteo.com/v1/air-quality"
        "?latitude=%.6f&longitude=%.6f&current=us_aqi&forecast_days=1", lat, lon);
    json = request_timeout(deadline) >= 1500 ? get_json(url, buffer, deadline) : NULL;
    next.aqi = entry(cJSON_GetObjectItemCaseSensitive(json, "current"), "us_aqi");
    cJSON_Delete(json);
    *result = next;
    return ESP_OK;
}

esp_err_t dashboard_weather_fetch(int64_t deadline)
{
    __atomic_store_n(&s_weather_live, false, __ATOMIC_RELEASE);
    if (!wifi_mgr_is_sta_up() || time(NULL) < EARLIEST_TIME) return ESP_ERR_INVALID_STATE;
    char *buffer = malloc(FORECAST_BYTES);
    if (!buffer) return ESP_ERR_NO_MEM;
    uint8_t bitmap[WEATHER_NAME_BYTES];
    char station[NMC_PATH_BYTES];
    double lat = 32.41, lon = 119.42;
    const uint32_t revision = __atomic_load_n(&s_location_revision, __ATOMIC_ACQUIRE);
    const esp_err_t location_err = dashboard_location_get(buffer, FORECAST_BYTES);
    const bool use_default = location_err == ESP_ERR_NVS_NOT_FOUND;
    if (use_default) {
        memcpy(station, NMC_DEFAULT_PATH, sizeof(NMC_DEFAULT_PATH));
    } else if (location_err != ESP_OK || !parse_location(buffer, &lat, &lon, bitmap, station)) {
        free(buffer);
        return ESP_ERR_INVALID_ARG;
    }
    weather_cache_t cached = {
        .location = location_hash(use_default ? dashboard_default_location() : buffer),
    };
    weather_data_t *next = &cached.weather;
    const esp_err_t err = station[0] ? load_nmc(station, buffer, next, deadline)
        : load_open_meteo(lat, lon, buffer, next, deadline);
    free(buffer);
    if (err != ESP_OK) return err;

    screen_rotation_lock();
    wifi_mgr_confirm_internet();
    if (revision != __atomic_load_n(&s_location_revision, __ATOMIC_ACQUIRE)) {
        screen_rotation_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    cached.fetched_at = (uint32_t)time(NULL);
    esp_err_t save_err = weather_sd_store(&cached);
    if (save_err == ESP_OK) ESP_LOGI(TAG, "Weather cache saved to " WEATHER_SD_FILE);
    else save_err = write_setting("weather_v2", &cached, sizeof(cached));
    if (save_err != ESP_OK) ESP_LOGW(TAG, "weather cache save failed: %s", esp_err_to_name(save_err));
    s_weather = *next;
    s_default_name = use_default;
    if (!use_default) memcpy(s_weather_name, bitmap, sizeof(s_weather_name));
    __atomic_store_n(&s_weather_ready, true, __ATOMIC_RELEASE);
    __atomic_store_n(&s_weather_live, true, __ATOMIC_RELEASE);
    screen_rotation_unlock();
    ESP_LOGI(TAG, "weather loaded from %s, %02d:%02d", next->nmc ? "NMC" : "Open-Meteo", next->hour, next->minute);
    return ESP_OK;
}

const weather_data_t *dashboard_weather_data(void)
{
    return &s_weather;
}

const uint8_t *dashboard_weather_name(void)
{
    return s_default_name ? NULL : s_weather_name;
}
