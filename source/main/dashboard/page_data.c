#include "page_data.h"
#include "net_sync.h"
#include "app_display.h"
#include "tf_card.h"
#include "wifi_mgr.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "page_data";
static SemaphoreHandle_t s_files;
static SemaphoreHandle_t s_trips_lock;
static unsigned s_shortcut = SCREEN_MODE_TODO;
extern const char lunar_2026[] asm("_binary_L2026_TSV_start");
extern const char lunar_2026_end[] asm("_binary_L2026_TSV_end");
extern const char lunar_2027[] asm("_binary_L2027_TSV_start");
extern const char lunar_2027_end[] asm("_binary_L2027_TSV_end");
extern const char history_seed[] asm("_binary_history_seed_tsv_start");
extern const char history_seed_end[] asm("_binary_history_seed_tsv_end");

static void paths(const char *name, char *path, char *temp, char *backup)
{
    const char *dot = strrchr(name, '.');
    const int stem = dot ? dot - name : strlen(name);
    snprintf(path, 32, TF_CARD_MOUNT_POINT "/%s", name);
    snprintf(temp, 32, TF_CARD_MOUNT_POINT "/%.*s.TMP", stem, name);
    snprintf(backup, 32, TF_CARD_MOUNT_POINT "/%.*s.BAK", stem, name);
}

static FILE *open_page(const char *name)
{
    char path[32], temp[32], backup[32];
    paths(name, path, temp, backup);
    FILE *file = fopen(path, "rb");
    return file ? file : fopen(backup, "rb");
}

esp_err_t page_sd_read(const char *name, char *data, size_t capacity)
{
    if (!s_files || !tf_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_files, portMAX_DELAY);
    FILE *file = open_page(name);
    esp_err_t result = ESP_ERR_NOT_FOUND;
    if (file) {
        const size_t n = fread(data, 1, capacity - 1, file);
        const int tail = fgetc(file);
        result = ferror(file) ? ESP_FAIL : tail != EOF ? ESP_ERR_INVALID_SIZE : ESP_OK;
        data[n] = 0;
        fclose(file);
    }
    xSemaphoreGive(s_files);
    return result;
}

static esp_err_t write_page(const char *name, const char *data, size_t bytes)
{
    if (!s_files || !tf_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    char path[32], temp[32], backup[32];
    paths(name, path, temp, backup);
    xSemaphoreTake(s_files, portMAX_DELAY);
    FILE *file = fopen(temp, "wb");
    bool ok = file && fwrite(data, 1, bytes, file) == bytes && fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (file && fclose(file) != 0) ok = false;
    struct stat st;
    if (ok && stat(path, &st) == 0) {
        remove(backup);
        ok = rename(path, backup) == 0;
    }
    if (ok) ok = rename(temp, path) == 0;
    if (ok) remove(backup);
    else {
        if (stat(path, &st) != 0) rename(backup, path);
        remove(temp);
    }
    xSemaphoreGive(s_files);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t page_sd_write(const char *name, const char *data)
{
    return write_page(name, data, strlen(data));
}

screen_mode_t page_shortcut_get(void)
{
    return __atomic_load_n(&s_shortcut, __ATOMIC_ACQUIRE);
}

esp_err_t page_shortcut_set(screen_mode_t mode)
{
    if (mode >= SCREEN_MODE_COUNT) return ESP_ERR_INVALID_ARG;
    char value[4];
    snprintf(value, sizeof(value), "%u", (unsigned)mode);
    const esp_err_t err = page_sd_write("PWRKEY.CFG", value);
    if (err == ESP_OK) __atomic_store_n(&s_shortcut, mode, __ATOMIC_RELEASE);
    return err;
}

static bool format_date(const char *date)
{
    if (!date || strlen(date) != 10 || date[4] != '-' || date[7] != '-') return false;
    for (size_t i = 0; i < 10; ++i)
        if (i != 4 && i != 7 && !isdigit((unsigned char)date[i])) return false;
    const int year = atoi(date), month = atoi(date + 5), day = atoi(date + 8);
    if (year < 2025 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31) return false;
    struct tm parsed = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day, .tm_hour = 12};
    mktime(&parsed);
    return parsed.tm_mon == month - 1 && parsed.tm_mday == day;
}

static bool format_time(const char *value)
{
    return strlen(value) == 5 && isdigit((unsigned char)value[0]) &&
        isdigit((unsigned char)value[1]) && value[2] == ':' &&
        isdigit((unsigned char)value[3]) && isdigit((unsigned char)value[4]) &&
        atoi(value) < 24 && atoi(value + 3) < 60;
}

bool page_deadline_valid(const char *value)
{
    if (!*value) return true;
    if (strlen(value) != 16 || value[10] != 'T') return false;
    char date[11];
    memcpy(date, value, 10);
    date[10] = 0;
    return format_date(date) && format_time(value + 11);
}

static bool copy_field(const cJSON *item, const char *key, char *out, size_t capacity, bool required)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(item, key);
    if (!field && !required) { out[0] = 0; return true; }
    if (!cJSON_IsString(field) || (required && !field->valuestring[0]) ||
            strlen(field->valuestring) >= capacity || strpbrk(field->valuestring, "\r\n\t")) return false;
    strcpy(out, field->valuestring);
    return true;
}

static bool parse_trip(const cJSON *item, page_trip_t *trip)
{
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(item, "latitude");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(item, "longitude");
    if (!(cJSON_IsNumber(lat) && cJSON_IsNumber(lon) &&
            lat->valuedouble >= -90 && lat->valuedouble <= 90 &&
            lon->valuedouble >= -180 && lon->valuedouble <= 180)) return false;
    trip->latitude = lat->valuedouble;
    trip->longitude = lon->valuedouble;
#define TRIP_FIELD(name, required) copy_field(item, #name, trip->name, sizeof(trip->name), required)
    return TRIP_FIELD(kind, true) && (!strcmp(trip->kind, "train") || !strcmp(trip->kind, "flight")) &&
        TRIP_FIELD(date, true) && format_date(trip->date) &&
        TRIP_FIELD(arrival_date, true) && format_date(trip->arrival_date) &&
        strcmp(trip->arrival_date, trip->date) >= 0 &&
        TRIP_FIELD(depart, true) && format_time(trip->depart) &&
        TRIP_FIELD(arrive, true) && format_time(trip->arrive) &&
        (strcmp(trip->arrival_date, trip->date) || strcmp(trip->arrive, trip->depart) >= 0) &&
        TRIP_FIELD(number, true) && TRIP_FIELD(from, true) && TRIP_FIELD(to, true) &&
        TRIP_FIELD(city, true) && TRIP_FIELD(seat, false) && TRIP_FIELD(gate, false) && TRIP_FIELD(note, false);
#undef TRIP_FIELD
}

static unsigned remove_expired_trips(cJSON *json, const struct tm *now)
{
    struct tm cutoff = *now;
    cutoff.tm_mday -= 7;
    mktime(&cutoff);
    char limit[11];
    strftime(limit, sizeof(limit), "%Y-%m-%d", &cutoff);
    unsigned removed = 0;
    for (int i = cJSON_GetArraySize(json) - 1; i >= 0; --i) {
        page_trip_t trip;
        if (parse_trip(cJSON_GetArrayItem(json, i), &trip) && strcmp(trip.arrival_date, limit) <= 0) {
            cJSON_DeleteItemFromArray(json, i);
            ++removed;
        }
    }
    return removed;
}

esp_err_t page_trips_set(const char *body)
{
    if (!s_trips_lock) return ESP_ERR_INVALID_STATE;
    cJSON *json = cJSON_ParseWithOpts(body, NULL, true);
    bool valid = cJSON_IsArray(json) && cJSON_GetArraySize(json) <= 12;
    const cJSON *item;
    page_trip_t trip;
    cJSON_ArrayForEach(item, json) {
        if (!valid || !parse_trip(item, &trip)) { valid = false; break; }
    }
    if (!valid) { cJSON_Delete(json); return ESP_ERR_INVALID_ARG; }
    struct tm now;
    char *filtered = NULL;
    if (dashboard_local_time(&now) && remove_expired_trips(json, &now)) {
        filtered = cJSON_PrintUnformatted(json);
        if (!filtered) { cJSON_Delete(json); return ESP_ERR_NO_MEM; }
    }
    cJSON_Delete(json);
    xSemaphoreTake(s_trips_lock, portMAX_DELAY);
    const esp_err_t err = page_sd_write("TRIPS.JSN", filtered ? filtered : body);
    xSemaphoreGive(s_trips_lock);
    free(filtered);
    return err;
}

static void trip_id(const page_trip_t *trip, char id[9])
{
    const char *fields[] = {trip->kind, trip->date, trip->depart, trip->number, trip->from, trip->to};
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        for (const unsigned char *p = (const unsigned char *)fields[i]; *p; ++p)
            hash = (hash ^ *p) * 16777619U;
        hash = (hash ^ 0xff) * 16777619U;
    }
    snprintf(id, 9, "%08x", (unsigned)hash);
}

static void trip_selection(char id[9])
{
    if (page_sd_read("TRIPSEL.CFG", id, 9) != ESP_OK) id[0] = 0;
}

esp_err_t page_trip_select(const char *id)
{
    if (!strcmp(id, "auto")) return page_sd_write("TRIPSEL.CFG", "");
    if (strlen(id) != 8 || strspn(id, "0123456789abcdef") != 8) return ESP_ERR_INVALID_ARG;
    char *body = malloc(TRIPS_BYTES);
    if (!body) return ESP_ERR_NO_MEM;
    cJSON *json = page_sd_read("TRIPS.JSN", body, TRIPS_BYTES) == ESP_OK ? cJSON_Parse(body) : NULL;
    free(body);
    bool found = false;
    const cJSON *item;
    cJSON_ArrayForEach(item, json) {
        page_trip_t trip;
        if (!parse_trip(item, &trip)) continue;
        char candidate[9];
        trip_id(&trip, candidate);
        if (!strcmp(candidate, id)) { found = true; break; }
    }
    cJSON_Delete(json);
    return found ? page_sd_write("TRIPSEL.CFG", id) : ESP_ERR_INVALID_ARG;
}

bool page_trip_get(const struct tm *now, page_trip_t *trip)
{
    char *body = malloc(TRIPS_BYTES);
    if (!body) return false;
    cJSON *json = page_sd_read("TRIPS.JSN", body, TRIPS_BYTES) == ESP_OK ? cJSON_Parse(body) : NULL;
    free(body);
    char today[18] = "";
    if (now) strftime(today, sizeof(today), "%Y-%m-%d %H:%M", now);
    char earliest[18] = "";
    bool earliest_today = false;
    char selected[9];
    trip_selection(selected);
    const cJSON *item;
    cJSON_ArrayForEach(item, json) {
        page_trip_t next;
        if (!parse_trip(item, &next)) continue;
        char id[9];
        trip_id(&next, id);
        if (!strcmp(selected, id)) {
            *trip = next;
            cJSON_Delete(json);
            return true;
        }
        char end[18], start[18];
        snprintf(end, sizeof(end), "%s %s", next.arrival_date, next.arrive);
        snprintf(start, sizeof(start), "%s %s", next.date, next.depart);
        const bool starts_today = now && !strncmp(next.date, today, 10);
        if (strcmp(end, today) >= 0 && (!earliest[0] ||
                (starts_today && !earliest_today) ||
                (starts_today == earliest_today && strcmp(start, earliest) < 0))) {
            *trip = next;
            strcpy(earliest, start);
            earliest_today = starts_today;
        }
    }
    cJSON_Delete(json);
    return earliest[0] != 0;
}

bool page_trip_prune(const struct tm *now)
{
    if (!s_trips_lock || !tf_card_is_mounted()) return false;
    char *body = malloc(TRIPS_BYTES);
    if (!body) return false;
    xSemaphoreTake(s_trips_lock, portMAX_DELAY);
    cJSON *json = page_sd_read("TRIPS.JSN", body, TRIPS_BYTES) == ESP_OK ? cJSON_Parse(body) : NULL;
    free(body);
    const unsigned removed = remove_expired_trips(json, now);
    bool stored = false;
    if (removed) {
        body = cJSON_PrintUnformatted(json);
        stored = body && page_sd_write("TRIPS.JSN", body) == ESP_OK;
        if (stored)
            ESP_LOGI(TAG, "Pruned %u expired SD trips", removed);
        free(body);
    }
    cJSON_Delete(json);
    xSemaphoreGive(s_trips_lock);
    return stored;
}

static bool split_lunar(char *row, page_lunar_t *lunar)
{
    char *date = strsep(&row, "\t");
    char *name = strsep(&row, "\t");
    char *yi = strsep(&row, "\t");
    char *ji = strsep(&row, "\t");
    if (!format_date(date) || !name || !*name || !yi || !*yi || !ji || !*ji || !row ||
        strlen(name) >= sizeof(lunar->lunar) || strlen(yi) >= sizeof(lunar->yi) ||
        strlen(ji) >= sizeof(lunar->ji) || strlen(row) >= sizeof(lunar->term) || strchr(row, '\t')) return false;
    strcpy(lunar->lunar, name);
    strcpy(lunar->yi, yi);
    strcpy(lunar->ji, ji);
    strcpy(lunar->term, row);
    return true;
}

esp_err_t page_lunar_upload(const char *content)
{
    if (!content || strlen(content) < 10) return ESP_ERR_INVALID_ARG;
    const int year = atoi(content);
    if (year < 2025 || year > 2099) return ESP_ERR_INVALID_ARG;
    const char *start = content;
    struct tm date = {.tm_year = year - 1900, .tm_mday = 1, .tm_hour = 12};
    char line[208], wanted[11];
    page_lunar_t sample;
    while (date.tm_year == year - 1900) {
        strftime(wanted, sizeof(wanted), "%Y-%m-%d", &date);
        const size_t len = strcspn(content, "\n");
        if (!content[len] || len >= sizeof(line) || strncmp(content, wanted, 10) || content[10] != '\t')
            return ESP_ERR_INVALID_ARG;
        memcpy(line, content, len);
        line[len] = 0;
        if (!split_lunar(line, &sample)) return ESP_ERR_INVALID_ARG;
        content += len + 1;
        ++date.tm_mday;
        mktime(&date);
    }
    if (*content) return ESP_ERR_INVALID_ARG;
    char name[16];
    snprintf(name, sizeof(name), "L%04d.TSV", year);
    return write_page(name, start, content - start);
}

bool page_lunar_get(const struct tm *now, page_lunar_t *lunar)
{
    if (!s_files || !tf_card_is_mounted()) return false;
    char name[16], date[11], line[208];
    strftime(name, sizeof(name), "L%Y.TSV", now);
    strftime(date, sizeof(date), "%Y-%m-%d", now);
    xSemaphoreTake(s_files, portMAX_DELAY);
    FILE *file = open_page(name);
    bool ok = false;
    if (file) {
        while (fgets(line, sizeof(line), file)) {
            if (strncmp(line, date, 10) || line[10] != '\t') continue;
            line[strcspn(line, "\r\n")] = 0;
            ok = split_lunar(line, lunar);
            break;
        }
        fclose(file);
    }
    xSemaphoreGive(s_files);
    return ok;
}

static void history_file(const struct tm *date, char *name, size_t capacity)
{
    strftime(name, capacity, "H%m%d.JSN", date);
}

static uint32_t history_crc(int year, const char *text, const char *date, uint32_t fetched)
{
    char header[64];
    const int size = snprintf(header, sizeof(header), "1|%d|%s|%lu|", year, date, (unsigned long)fetched);
    const uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)header, size);
    return esp_rom_crc32_le(crc, (const uint8_t *)text, strlen(text));
}

bool page_history_get(const struct tm *now, page_history_t *history)
{
    char name[16], body[768];
    history_file(now, name, sizeof(name));
    if (page_sd_read(name, body, sizeof(body)) != ESP_OK) return false;
    cJSON *json = cJSON_Parse(body);
    const cJSON *year = cJSON_GetObjectItemCaseSensitive(json, "year");
    bool ok = cJSON_IsNumber(year) && year->valueint >= -5000 && year->valueint <= now->tm_year + 1900 &&
        copy_field(json, "text", history->text, sizeof(history->text), true);
    if (ok) {
        history->year = year->valueint;
        history->fetched_at = 0;
        const cJSON *source = cJSON_GetObjectItemCaseSensitive(json, "source");
        history->domestic_source = cJSON_IsString(source) && !strcmp(source->valuestring, "apihz");
        strftime(history->date, sizeof(history->date), "%Y-%m-%d", now);
        const cJSON *schema = cJSON_GetObjectItemCaseSensitive(json, "schema_version");
        const cJSON *fetched = cJSON_GetObjectItemCaseSensitive(json, "fetched_at");
        const cJSON *valid = cJSON_GetObjectItemCaseSensitive(json, "valid_for_date");
        const cJSON *crc = cJSON_GetObjectItemCaseSensitive(json, "crc");
        if (schema || fetched || valid || crc) {
            ok = cJSON_IsNumber(schema) && schema->valuedouble == 1 &&
                cJSON_IsString(valid) && !strcmp(valid->valuestring, history->date) &&
                cJSON_IsNumber(fetched) && fetched->valuedouble > 0 && fetched->valuedouble <= UINT32_MAX &&
                cJSON_IsNumber(crc) && crc->valuedouble ==
                    history_crc(history->year, history->text, history->date, (uint32_t)fetched->valuedouble);
            if (ok) history->fetched_at = (uint32_t)fetched->valuedouble;
        }
    }
    cJSON_Delete(json);
    return ok;
}

/* 国内接口的日期字段可能是数字，也可能是十进制字符串。 */
static int history_number(const cJSON *value)
{
    if (cJSON_IsNumber(value) && value->valuedouble == value->valueint) return value->valueint;
    if (!cJSON_IsString(value) || !value->valuestring[0]) return 0;
    const char *start = value->valuestring;
    char *end;
    const long n = strtol(start, &end, 10);
    return end != start && !*end && n >= -5000 && n <= 9999 ? (int)n : 0;
}

static esp_http_client_handle_t open_url(const char *url)
{
    const esp_http_client_config_t config = {
        .url = url, .timeout_ms = 3500, .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "HueLeafFirmware/1.0 (+https://gitee.com/osptek/devkit-hueleaf_base)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;
    if (esp_http_client_open(client, 0) != ESP_OK || esp_http_client_fetch_headers(client) < 0 ||
            esp_http_client_get_status_code(client) != 200) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    wifi_mgr_confirm_internet();
    return client;
}

typedef struct {
    char id[21];
    char key[65];
} history_credentials_t;

esp_err_t page_history_set_credentials(const char *id, const char *key)
{
    if (!id || !key || strlen(id) >= 21 || strlen(key) >= 65 ||
            (!!*id != !!*key) || strspn(id, "0123456789") != strlen(id) ||
            strspn(key, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_") != strlen(key))
        return ESP_ERR_INVALID_ARG;
    history_credentials_t credentials = {0};
    strcpy(credentials.id, id);
    strcpy(credentials.key, key);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("history", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "apihz_v1", &credentials, sizeof(credentials));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) net_sync_request(NET_SYNC_HISTORY | NET_SYNC_MANUAL);
    return err;
}

static bool fetch_history(const struct tm *date)
{
    char url[176], name[16], response[1536];
    history_credentials_t credentials = {0};
    nvs_handle_t handle;
    if (nvs_open("history", NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(credentials);
        if (nvs_get_blob(handle, "apihz_v1", &credentials, &size) != ESP_OK ||
                size != sizeof(credentials)) memset(&credentials, 0, sizeof(credentials));
        nvs_close(handle);
    }
    credentials.id[sizeof(credentials.id) - 1] = 0;
    credentials.key[sizeof(credentials.key) - 1] = 0;
    snprintf(url, sizeof(url), "https://cn.apihz.cn/api/zici/today.php?id=%s&key=%s&m=%d&d=%d",
        credentials.id[0] && credentials.key[0] ? credentials.id : "88888888",
        credentials.id[0] && credentials.key[0] ? credentials.key : "88888888",
        date->tm_mon + 1, date->tm_mday);
    esp_http_client_handle_t client = open_url(url);
    if (!client) return false;
    const int received = esp_http_client_read_response(client, response, sizeof(response) - 1);
    esp_http_client_cleanup(client);
    if (received <= 0 || received >= sizeof(response) - 1) return false;
    response[received] = 0;
    cJSON *result = cJSON_ParseWithOpts(response, NULL, true);
    const int year = history_number(cJSON_GetObjectItemCaseSensitive(result, "y"));
    char title[273];
    const bool found = history_number(cJSON_GetObjectItemCaseSensitive(result, "code")) == 200 &&
        history_number(cJSON_GetObjectItemCaseSensitive(result, "m")) == date->tm_mon + 1 &&
        history_number(cJSON_GetObjectItemCaseSensitive(result, "d")) == date->tm_mday &&
        year != 0 && year >= -5000 && year <= date->tm_year + 1900 &&
        copy_field(result, "title", title, sizeof(title), true);
    cJSON_Delete(result);
    if (!found) {
        ESP_LOGW(TAG, "国内历史接口未返回有效事件 %02d-%02d", date->tm_mon + 1, date->tm_mday);
        return false;
    }
    cJSON *json = cJSON_CreateObject();
    if (!json) return false;
    char valid_for[11];
    strftime(valid_for, sizeof(valid_for), "%Y-%m-%d", date);
    const uint32_t fetched = (uint32_t)time(NULL);
    if (!cJSON_AddNumberToObject(json, "year", year) ||
            !cJSON_AddStringToObject(json, "text", title) ||
            !cJSON_AddStringToObject(json, "source", "apihz") ||
            !cJSON_AddNumberToObject(json, "schema_version", 1) ||
            !cJSON_AddStringToObject(json, "valid_for_date", valid_for) ||
            !cJSON_AddNumberToObject(json, "fetched_at", fetched) ||
            !cJSON_AddNumberToObject(json, "crc", history_crc(year, title, valid_for, fetched))) {
        cJSON_Delete(json);
        return false;
    }
    char *body = cJSON_PrintUnformatted(json);
    history_file(date, name, sizeof(name));
    const bool saved = body && page_sd_write(name, body) == ESP_OK;
    free(body);
    cJSON_Delete(json);
    return saved;
}

static void trip_weather_key(const page_trip_t *trip, char *key, size_t capacity)
{
    snprintf(key, capacity, "%s,%0.4f,%0.4f", trip->arrival_date, trip->latitude, trip->longitude);
}

static void trip_weather_file(const char *key, char *name, size_t capacity)
{
    uint32_t hash = 2166136261U;
    while (*key) hash = (hash ^ (uint8_t)*key++) * 16777619U;
    snprintf(name, capacity, "W%07X.TSV", (unsigned)(hash & 0x0fffffffU));
}

static bool read_trip_weather(const page_trip_t *trip, page_trip_weather_t *weather, char *saved_date)
{
    char body[192], key[64], name[16];
    trip_weather_key(trip, key, sizeof(key));
    trip_weather_file(key, name, sizeof(name));
    if (page_sd_read(name, body, sizeof(body)) != ESP_OK) return false;
    char *metadata = strchr(body, '|');
    if (metadata) {
        unsigned long fetched, crc;
        int end = 0;
        *metadata++ = 0;
        if (sscanf(metadata, "1 %lu %lx%n", &fetched, &crc, &end) != 2 || metadata[end] ||
                fetched == 0 || crc != esp_rom_crc32_le(0, (const uint8_t *)body, strlen(body))) return false;
    }
    char *cursor = strchr(body, '\t');
    if (!cursor) return false;
    *cursor++ = 0;
    int end = 0;
    return !strcmp(body, key) && sscanf(cursor, "%10s %d %d %d %d%n", saved_date,
        &weather->high, &weather->low, &weather->rain, &weather->code, &end) == 5 &&
        !cursor[end] && format_date(saved_date);
}

bool page_trip_weather(const page_trip_t *trip, page_trip_weather_t *weather)
{
    return read_trip_weather(trip, weather, weather->updated);
}

char *page_trips_with_status(const char *body)
{
    cJSON *json = cJSON_Parse(body);
    if (!cJSON_IsArray(json)) { cJSON_Delete(json); return NULL; }
    char selected[9];
    trip_selection(selected);
    cJSON *item;
    cJSON_ArrayForEach(item, json) {
        page_trip_t trip;
        page_trip_weather_t weather;
        if (!parse_trip(item, &trip)) continue;
        char id[9];
        trip_id(&trip, id);
        const bool cached = page_trip_weather(&trip, &weather);
        cJSON_DeleteItemFromObjectCaseSensitive(item, "weather_cached");
        cJSON_DeleteItemFromObjectCaseSensitive(item, "weather_updated");
        cJSON_AddStringToObject(item, "trip_id", id);
        cJSON_AddBoolToObject(item, "selected", !strcmp(selected, id));
        cJSON_AddBoolToObject(item, "weather_cached", cached);
        if (cached) cJSON_AddStringToObject(item, "weather_updated", weather.updated);
    }
    char *status = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return status;
}

static esp_err_t fetch_trip_weather(const page_trip_t *trip, const char *today, bool refresh, bool *changed)
{
    page_trip_weather_t cached;
    char saved[11];
    const bool has_cache = read_trip_weather(trip, &cached, saved);
    *changed = false;
    if (!refresh && has_cache && !strcmp(saved, today)) return ESP_OK;
    char url[320];
    snprintf(url, sizeof(url), "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,weather_code"
        "&timezone=auto&start_date=%s&end_date=%s", trip->latitude, trip->longitude, trip->arrival_date, trip->arrival_date);
    esp_http_client_handle_t client = open_url(url);
    if (!client) return ESP_FAIL;
    char response[1536];
    const int n = esp_http_client_read_response(client, response, sizeof(response) - 1);
    esp_http_client_cleanup(client);
    if (n <= 0 || n >= sizeof(response) - 1) return ESP_ERR_INVALID_SIZE;
    response[n] = 0;
    cJSON *json = cJSON_Parse(response);
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(json, "daily");
    const cJSON *dates = cJSON_GetObjectItemCaseSensitive(daily, "time");
    int day_index = 0;
    const cJSON *date;
    cJSON_ArrayForEach(date, dates) {
        if (cJSON_IsString(date) && !strcmp(date->valuestring, trip->arrival_date)) break;
        ++day_index;
    }
    if (!cJSON_IsArray(dates) || !date) {
        ESP_LOGW(TAG, "Forecast response missing arrival date %s", trip->arrival_date);
        cJSON_Delete(json);
        return ESP_ERR_NOT_FOUND;
    }
    const cJSON *high = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max"), day_index);
    const cJSON *low = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min"), day_index);
    const cJSON *rain = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "precipitation_probability_max"), day_index);
    const cJSON *code = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "weather_code"), day_index);
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    if (cJSON_IsNumber(high) && cJSON_IsNumber(low) && cJSON_IsNumber(code)) {
        char key[64], body[192], name[16];
        trip_weather_key(trip, key, sizeof(key));
        trip_weather_file(key, name, sizeof(name));
        const int rain_chance = cJSON_IsNumber(rain) ? rain->valueint : -1;
        snprintf(body, sizeof(body), "%s\t%s %d %d %d %d", key, today,
            high->valueint, low->valueint, rain_chance, code->valueint);
        const size_t payload_size = strlen(body);
        const uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)body, payload_size);
        snprintf(body + payload_size, sizeof(body) - payload_size, "|1 %lu %08lx",
            (unsigned long)time(NULL), (unsigned long)crc);
        result = page_sd_write(name, body);
        if (result == ESP_OK) {
            *changed = !has_cache || cached.high != high->valueint || cached.low != low->valueint ||
                cached.rain != rain_chance || cached.code != code->valueint;
            ESP_LOGI(TAG, "Saved destination forecast for %s", trip->arrival_date);
        }
    }
    cJSON_Delete(json);
    return result;
}

static esp_err_t fetch_trips(const struct tm *now, const char *today)
{
    char *body = malloc(TRIPS_BYTES);
    if (!body) return ESP_ERR_NO_MEM;
    const esp_err_t read = page_sd_read("TRIPS.JSN", body, TRIPS_BYTES);
    cJSON *json = read == ESP_OK ? cJSON_Parse(body) : NULL;
    free(body);
    if (read == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (read != ESP_OK) return read;
    if (!cJSON_IsArray(json)) { cJSON_Delete(json); return ESP_ERR_INVALID_RESPONSE; }
    esp_err_t result = ESP_OK;
    struct tm final_day = *now;
    final_day.tm_mday += 7;
    mktime(&final_day);
    char end[11];
    strftime(end, sizeof(end), "%Y-%m-%d", &final_day);
    const cJSON *item;
    page_trip_t featured;
    char featured_key[64] = "";
    if (page_trip_get(now, &featured)) trip_weather_key(&featured, featured_key, sizeof(featured_key));
    bool visible_forecast_changed = false;
    const int64_t deadline = esp_timer_get_time() + 25000000;
    cJSON_ArrayForEach(item, json) {
        if (!wifi_mgr_is_sta_up() || esp_timer_get_time() >= deadline || net_sync_expired()) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        page_trip_t trip;
        if (parse_trip(item, &trip) && strcmp(trip.arrival_date, today) >= 0 &&
                strcmp(trip.arrival_date, end) <= 0) {
            bool changed;
            const esp_err_t err = fetch_trip_weather(&trip, today, false, &changed);
            if (err != ESP_OK) result = err;
            if (!changed) continue;
            char key[64];
            trip_weather_key(&trip, key, sizeof(key));
            visible_forecast_changed |= !strcmp(key, featured_key);
        }
    }
    cJSON_Delete(json);
    if (visible_forecast_changed) app_display_trip_data_changed();
    return result;
}

void page_network_refresh(void)
{
    net_sync_request(NET_SYNC_TRIPS);
}

esp_err_t page_sync_history(struct tm date)
{
    if (!tf_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    unsigned stored = 0;
    for (int i = 0; i < 7 && wifi_mgr_is_sta_up() && !net_sync_expired(); ++i) {
        page_history_t existing;
        if (page_history_get(&date, &existing)) {
            ++stored;
        } else if (fetch_history(&date)) {
            ++stored;
            if (i == 0) app_display_history_data_changed();
        }
        ++date.tm_mday;
        mktime(&date);
    }
    ESP_LOGI(TAG, "未来七天历史缓存已覆盖 %u 天", stored);
    return stored == 7 ? ESP_OK : ESP_ERR_NOT_FINISHED;
}

esp_err_t page_sync_trips(const struct tm *date, bool opened)
{
    if (!tf_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    char day[11];
    strftime(day, sizeof(day), "%Y-%m-%d", date);
    if (!opened) return fetch_trips(date, day);
    page_trip_t trip;
    if (!page_trip_get(date, &trip) || strcmp(trip.arrival_date, day)) return ESP_OK;
    bool changed;
    const esp_err_t result = fetch_trip_weather(&trip, day, true, &changed);
    if (changed) app_display_trip_data_changed();
    return result;
}

void page_trip_opened(void)
{
    if (wifi_mgr_is_sta_up()) net_sync_request(NET_SYNC_TRIP_OPEN);
}

esp_err_t page_data_init(void)
{
    s_files = xSemaphoreCreateMutex();
    s_trips_lock = xSemaphoreCreateMutex();
    if (!s_files || !s_trips_lock) return ESP_ERR_NO_MEM;
    if (tf_card_is_mounted()) {
        char setting[8];
        if (page_sd_read("PWRKEY.CFG", setting, sizeof(setting)) == ESP_OK &&
                strlen(setting) == 1 && setting[0] >= '0' && setting[0] < '0' + SCREEN_MODE_COUNT)
            s_shortcut = setting[0] - '0';
        const char *names[] = {"L2026.TSV", "L2027.TSV"};
        const char *start[] = {lunar_2026, lunar_2027};
        const char *end[] = {lunar_2026_end, lunar_2027_end};
        for (int i = 0; i < 2; ++i) {
            FILE *file = open_page(names[i]);
            if (file) fclose(file);
            else if (write_page(names[i], start[i], end[i] - start[i]) == ESP_OK)
                ESP_LOGI(TAG, "Seeded annual SD calendar %s", names[i]);
        }
        unsigned seeded = 0;
        for (const char *cursor = history_seed; cursor < history_seed_end;) {
            const char *end = memchr(cursor, '\n', history_seed_end - cursor);
            if (!end || end - cursor < 6 || cursor[4] != '\t') break;
            char name[16];
            snprintf(name, sizeof(name), "H%.4s.JSN", cursor);
            FILE *file = open_page(name);
            if (file) fclose(file);
            else if (write_page(name, cursor + 5, end - cursor - 5) == ESP_OK) ++seeded;
            cursor = end + 1;
        }
        if (seeded) ESP_LOGI(TAG, "Seeded %u Wikimedia SD history dates", seeded);
    }
    return ESP_OK;
}
