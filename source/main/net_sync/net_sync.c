#include "net_sync.h"
#include "app_display.h"
#include "axp2101.h"
#include "dashboard.h"
#include "page_data.h"
#include "wifi_mgr.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>

#define SECOND 1000000LL
#define SYNC_CLOCK (1u << 5)
static const char *TAG = "net_sync";
static TaskHandle_t s_task;
static SemaphoreHandle_t s_radio;
static bool s_busy, s_management;
static int64_t s_deadline;
static uint32_t s_last_weather, s_last_history, s_last_trips, s_last_clock;
static int s_error;
#define DAILY_OFF (1u << 31)
#define AUTO_CLOSE (1u << 15)
static uint32_t s_window = (210u << 16) | 330u;
static uint32_t s_retry_epoch, s_running_tasks, s_radio_seconds;
typedef struct { uint32_t at, tasks; int result; } sync_record_t;
static sync_record_t s_records[50];
static unsigned s_record_count, s_record_next;
static portMUX_TYPE s_records_lock = portMUX_INITIALIZER_UNLOCKED;
static void run(void *arg);

esp_err_t net_sync_start(void)
{
    nvs_handle_t settings;
    if (nvs_open("sync", NVS_READONLY, &settings) == ESP_OK) {
        uint32_t value;
        if (nvs_get_u32(settings, "window_v1", &value) == ESP_OK &&
                ((value >> 16) & 2047u) < 1440 && (value & 2047u) < 1440) s_window = value;
        (void)nvs_get_u32(settings, "weather_ok", &s_last_weather);
        (void)nvs_get_u32(settings, "history_ok", &s_last_history);
        (void)nvs_get_u32(settings, "trips_ok", &s_last_trips);
        (void)nvs_get_u32(settings, "clock_ok", &s_last_clock);
        nvs_close(settings);
    }
    s_radio = xSemaphoreCreateMutex();
    if (!s_radio) return ESP_ERR_NO_MEM;
    return xTaskCreate(run, "net_sync", 10240, NULL, 2, &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void net_sync_request(uint32_t tasks)
{
    if (s_task) xTaskNotify(s_task, tasks, eSetBits);
}

void net_sync_clock_valid(void)
{
    net_sync_request(SYNC_CLOCK);
}

bool net_sync_expired(void)
{
    return esp_timer_get_time() >= s_deadline;
}

bool net_sync_busy(void)
{
    return __atomic_load_n(&s_busy, __ATOMIC_ACQUIRE);
}

esp_err_t net_sync_open_management(void)
{
    xSemaphoreTake(s_radio, portMAX_DELAY);
    const esp_err_t err = wifi_mgr_show_hotspot();
    if (err == ESP_OK && !s_management) {
        wifi_mgr_acquire(WIFI_LEASE_MANAGEMENT);
        s_management = true;
    }
    xSemaphoreGive(s_radio);
    return err;
}

bool net_sync_close_management(void)
{
    xSemaphoreTake(s_radio, portMAX_DELAY);
    bool idle = !net_sync_busy();
    if (idle && s_management) {
        wifi_mgr_release(WIFI_LEASE_MANAGEMENT);
        idle = !wifi_mgr_has_leases();
        if (idle) {
            s_management = false;
            wifi_mgr_stop();
        } else {
            wifi_mgr_acquire(WIFI_LEASE_MANAGEMENT);
        }
    }
    xSemaphoreGive(s_radio);
    return idle;
}

esp_err_t net_sync_set_window(unsigned start, unsigned end, int daily, int auto_close)
{
    if (start >= 1440 || end >= 1440 || start == end) return ESP_ERR_INVALID_ARG;
    nvs_handle_t settings;
    esp_err_t err = nvs_open("sync", NVS_READWRITE, &settings);
    if (err != ESP_OK) return err;
    const uint32_t old = __atomic_load_n(&s_window, __ATOMIC_ACQUIRE);
    const uint32_t flags = (daily < 0 ? old & DAILY_OFF : daily ? 0 : DAILY_OFF) |
        (auto_close < 0 ? old & AUTO_CLOSE : auto_close ? AUTO_CLOSE : 0);
    const uint32_t value = (start << 16) | end | flags;
    err = nvs_set_u32(settings, "window_v1", value);
    if (err == ESP_OK) err = nvs_commit(settings);
    nvs_close(settings);
    if (err == ESP_OK) __atomic_store_n(&s_window, value, __ATOMIC_RELEASE);
    return err;
}

bool net_sync_auto_close(void)
{
    return (__atomic_load_n(&s_window, __ATOMIC_ACQUIRE) & AUTO_CLOSE) != 0;
}

int net_sync_status(char *out, size_t size)
{
    sync_record_t records[50];
    portENTER_CRITICAL(&s_records_lock);
    const unsigned count = s_record_count;
    for (unsigned i = 0; i < count; ++i)
        records[i] = s_records[(s_record_next + 49 - i) % 50];
    portEXIT_CRITICAL(&s_records_lock);
    const uint32_t window = __atomic_load_n(&s_window, __ATOMIC_ACQUIRE);
    int used = snprintf(out, size,
        "{\"busy\":%s,\"clock\":%lu,\"weather\":%lu,\"history\":%lu,\"trips\":%lu,\"error\":%d,\"retry\":%lu,\"start\":%lu,\"end\":%lu,\"daily\":%s,\"auto_close\":%s,\"tasks\":%lu,\"radio_seconds\":%lu,\"logs\":[",
        net_sync_busy() ? "true" : "false",
        (unsigned long)__atomic_load_n(&s_last_clock, __ATOMIC_RELAXED),
        (unsigned long)__atomic_load_n(&s_last_weather, __ATOMIC_RELAXED),
        (unsigned long)__atomic_load_n(&s_last_history, __ATOMIC_RELAXED),
        (unsigned long)__atomic_load_n(&s_last_trips, __ATOMIC_RELAXED),
        __atomic_load_n(&s_error, __ATOMIC_RELAXED),
        (unsigned long)__atomic_load_n(&s_retry_epoch, __ATOMIC_RELAXED),
        (unsigned long)((window >> 16) & 2047u), (unsigned long)(window & 2047u),
        window & DAILY_OFF ? "false" : "true", window & AUTO_CLOSE ? "true" : "false",
        (unsigned long)__atomic_load_n(&s_running_tasks, __ATOMIC_RELAXED),
        (unsigned long)__atomic_load_n(&s_radio_seconds, __ATOMIC_RELAXED));
    if (used < 0 || (size_t)used >= size) return -1;
    for (unsigned i = 0; i < count; ++i) {
        const int n = snprintf(out + used, size - used, "%s[%lu,%lu,%d]",
            i ? "," : "", (unsigned long)records[i].at, (unsigned long)records[i].tasks, records[i].result);
        if (n < 0 || (size_t)n >= size - used) return -1;
        used += n;
    }
    return snprintf(out + used, size - used, "]}") == 2 && size - used > 2 ? used + 2 : -1;
}

static void note_success(const char *key, uint32_t *stamp)
{
    const uint32_t now = (uint32_t)time(NULL);
    const uint32_t previous = __atomic_exchange_n(stamp, now, __ATOMIC_RELAXED);
    // 一天内只更新内存时间，避免翻页触发反复写 NVS。
    if (previous && (previous + 28800u) / 86400u == (now + 28800u) / 86400u) return;
    nvs_handle_t settings;
    if (nvs_open("sync", NVS_READWRITE, &settings) != ESP_OK) return;
    if (nvs_set_u32(settings, key, now) == ESP_OK) (void)nvs_commit(settings);
    nvs_close(settings);
}

typedef struct {
    uint32_t bit;
    uint32_t last_success_day;
    unsigned fail_count;
    int64_t next_retry_at;
} sync_task_state_t;

static esp_err_t run_job(uint32_t bit, const struct tm *date, bool manual)
{
    if (!wifi_mgr_is_sta_up() || net_sync_expired()) return ESP_ERR_TIMEOUT;
    if (bit == NET_SYNC_HISTORY) {
        const esp_err_t err = page_sync_history(*date);
        if (err == ESP_OK) note_success("history_ok", &s_last_history);
        return err;
    }
    if (dashboard_weather_offline() && !manual) return ESP_OK;
    if (bit == NET_SYNC_WEATHER) {
        if (!manual && dashboard_weather_ready() && !dashboard_weather_due(false)) return ESP_OK;
        bool changed = false;
        const esp_err_t err = dashboard_weather_fetch(s_deadline, &changed);
        if (changed) app_display_weather_data_changed();
        if (err == ESP_OK) {
            dashboard_weather_due(true);
            note_success("weather_ok", &s_last_weather);
        }
        return err;
    }
    const esp_err_t err = page_sync_trips(date, bit == NET_SYNC_TRIP_OPEN);
    if (err == ESP_OK && bit == NET_SYNC_TRIPS) note_success("trips_ok", &s_last_trips);
    return err;
}

static void record_job(uint32_t bit, esp_err_t result)
{
    const uint32_t at = (uint32_t)time(NULL);
    portENTER_CRITICAL(&s_records_lock);
    s_records[s_record_next] = (sync_record_t){at, bit, result};
    s_record_next = (s_record_next + 1) % 50;
    if (s_record_count < 50) ++s_record_count;
    portEXIT_CRITICAL(&s_records_lock);
}

static void run(void *arg)
{
    (void)arg;
    sync_task_state_t jobs[] = {
        {.bit = NET_SYNC_TRIP_OPEN}, {.bit = NET_SYNC_WEATHER},
        {.bit = NET_SYNC_TRIPS}, {.bit = NET_SYNC_HISTORY},
    };
    uint32_t pending = 0;
    int last_day = -1, window_day = -1;
    int64_t scheduled_until = 0;
    const unsigned delays[] = {300, 1800, 7200};
    for (;;) {
        uint32_t requests = 0;
        xTaskNotifyWait(0, UINT32_MAX, &requests, pdMS_TO_TICKS(30000));
        struct tm date;
        const bool valid = dashboard_local_time(&date);
        const int day_key = valid ? date.tm_year * 400 + date.tm_yday : -1;
        const bool new_day = valid && day_key != last_day;
        if (new_day) {
            last_day = day_key;
            pending |= NET_SYNC_ALL;
            if (page_trip_prune(&date)) app_display_trip_data_changed();
        }
        const uint32_t window = __atomic_load_n(&s_window, __ATOMIC_ACQUIRE);
        const int64_t tick = esp_timer_get_time();
        if (scheduled_until && (tick >= scheduled_until || (window & DAILY_OFF))) {
            xSemaphoreTake(s_radio, portMAX_DELAY);
            if (!s_management) wifi_mgr_stop();
            xSemaphoreGive(s_radio);
            scheduled_until = 0;
        }
        if (valid && !(window & DAILY_OFF) && window_day != day_key && wifi_mgr_has_credentials() &&
                axp2101_low_battery_percent() < 0) {
            const int start = (window >> 16) & 2047u, end = window & 2047u;
            const int minute = date.tm_hour * 60 + date.tm_min;
            if (start < end ? minute >= start && minute < end : minute >= start || minute < end) {
                window_day = day_key;
                scheduled_until = tick + 15 * 60 * SECOND;
                requests |= NET_SYNC_LINK | NET_SYNC_ALL;
                for (unsigned i = 0; i < sizeof(jobs)/sizeof(jobs[0]); ++i)
                    if ((jobs[i].bit & NET_SYNC_ALL) && jobs[i].last_success_day != (uint32_t)day_key)
                        pending |= jobs[i].bit;
            }
        }
        pending |= requests & (NET_SYNC_ALL | NET_SYNC_TRIP_OPEN | NET_SYNC_MANUAL);
        if (!valid) pending |= NET_SYNC_ALL;
        const bool manual = (pending & NET_SYNC_MANUAL) != 0;
        if ((!wifi_mgr_is_sta_up() && !wifi_mgr_has_credentials()) ||
                (!manual && axp2101_low_battery_percent() >= 0)) continue;
        const int64_t started = esp_timer_get_time();
        const bool restored = (requests & (SYNC_CLOCK | NET_SYNC_LINK)) != 0;
        uint32_t attempted = 0;
        for (unsigned i = 0; i < sizeof(jobs)/sizeof(jobs[0]); ++i) {
            sync_task_state_t *job = &jobs[i];
            if (new_day) {
                job->next_retry_at = 0;
                job->fail_count = 0;
            }
            const bool reopened = job->bit == NET_SYNC_TRIP_OPEN && (requests & NET_SYNC_TRIP_OPEN);
            if ((pending & job->bit) && (manual || restored || reopened || started >= job->next_retry_at))
                attempted |= job->bit;
        }
        if (!attempted) continue;

        __atomic_store_n(&s_running_tasks, attempted, __ATOMIC_RELAXED);
        xSemaphoreTake(s_radio, portMAX_DELAY);
        wifi_mgr_acquire(WIFI_LEASE_SYNC);
        __atomic_store_n(&s_busy, true, __ATOMIC_RELEASE);
        s_deadline = esp_timer_get_time() + 60 * SECOND;
        esp_err_t connection = wifi_mgr_weather_connect(8000);
        xSemaphoreGive(s_radio);
        bool sntp = false;
        if (connection == ESP_OK) {
            sntp = dashboard_clock_sync_start();
            if (sntp) {
                const esp_err_t clock = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
                if (clock == ESP_OK) note_success("clock_ok", &s_last_clock);
                record_job(SYNC_CLOCK, clock);
            }
            if (!dashboard_local_time(&date)) connection = ESP_ERR_INVALID_STATE;
        }
        const bool clock_valid = dashboard_local_time(&date);
        esp_err_t batch_result = connection;
        int64_t next_retry = INT64_MAX;
        for (unsigned i = 0; i < sizeof(jobs)/sizeof(jobs[0]); ++i) {
            sync_task_state_t *job = &jobs[i];
            if (attempted & job->bit) {
                const esp_err_t err = connection == ESP_OK ? run_job(job->bit, &date, manual) : connection;
                record_job(job->bit, err);
                if (err == ESP_OK) {
                    pending &= ~job->bit;
                    job->last_success_day = (uint32_t)(date.tm_year * 400 + date.tm_yday);
                    job->fail_count = 0;
                    job->next_retry_at = 0;
                } else {
                    if (batch_result == ESP_OK) batch_result = err;
                    const unsigned index = job->fail_count < 3 ? job->fail_count : 2;
                    if (job->fail_count < 4) ++job->fail_count;
                    const bool capped = job->fail_count >= 4 && clock_valid;
                    const unsigned wait = capped ?
                        86400 - date.tm_hour * 3600 - date.tm_min * 60 - date.tm_sec : delays[index];
                    job->next_retry_at = esp_timer_get_time() + wait * SECOND;
                }
            }
            if ((pending & job->bit) && job->next_retry_at < next_retry) next_retry = job->next_retry_at;
        }
        if (sntp) dashboard_clock_sync_stop();
        pending &= ~NET_SYNC_MANUAL;
        const int64_t now = esp_timer_get_time();
        __atomic_store_n(&s_error, batch_result, __ATOMIC_RELAXED);
        __atomic_store_n(&s_retry_epoch, clock_valid && next_retry != INT64_MAX && next_retry > now ?
            (uint32_t)time(NULL) + (uint32_t)((next_retry - now) / SECOND) : 0, __ATOMIC_RELAXED);
        ESP_LOGI(TAG, "同步结束，待处理 0x%lx，结果 %s", (unsigned long)pending, esp_err_to_name(batch_result));
        xSemaphoreTake(s_radio, portMAX_DELAY);
        const bool temporary = !s_management;
        xSemaphoreGive(s_radio);
        if (temporary && !net_sync_expired()) vTaskDelay(pdMS_TO_TICKS(10000));
        xSemaphoreTake(s_radio, portMAX_DELAY);
        wifi_mgr_release(WIFI_LEASE_SYNC);
        if (!s_management && (!scheduled_until || esp_timer_get_time() >= scheduled_until)) wifi_mgr_stop();
        __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
        __atomic_store_n(&s_running_tasks, 0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_radio_seconds, (uint32_t)((esp_timer_get_time() - started) / SECOND),
            __ATOMIC_RELAXED);
        xSemaphoreGive(s_radio);
        ESP_LOGD(TAG, "同步任务栈余量 %u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        app_display_schedule_changed();
    }
}
