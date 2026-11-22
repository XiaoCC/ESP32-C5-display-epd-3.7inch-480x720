#ifndef DASHBOARD_H
#define DASHBOARD_H

#include "screen_rotation.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define WEATHER_LOCATION_BYTES 1536
#define WEATHER_NAME_WIDTH 192
#define WEATHER_NAME_HEIGHT 32
#define WEATHER_NAME_BYTES (WEATHER_NAME_WIDTH * WEATHER_NAME_HEIGHT / 8)

typedef struct {
    uint8_t enabled;
    bool running;
    int16_t every;
} dashboard_schedule_t;

void dashboard_schedule_get(dashboard_schedule_t *schedule);
void dashboard_display_style_restore(void);
uint8_t dashboard_mono_mask(void);
esp_err_t dashboard_mono_set(uint8_t mask);
esp_err_t dashboard_schedule_set(const dashboard_schedule_t *schedule);
esp_err_t dashboard_location_get(char *body, size_t size);
esp_err_t dashboard_location_set(const char *body);
void dashboard_clock_start(void);
bool dashboard_local_time(struct tm *now);
bool dashboard_quote_due(void);
void dashboard_quote_init(void);
#define QUOTE_IMPORT_BYTES 8192
esp_err_t dashboard_quote_export(char **content, size_t *length);
esp_err_t dashboard_quote_import(char *content, unsigned *bad_row);
esp_err_t dashboard_quote_preview(const char *content);

#define TODO_MAX_ITEMS 5
#define TODO_TEXT_WIDTH 352
#define TODO_TEXT_HEIGHT 24
#define TODO_ROW_BYTES (TODO_TEXT_WIDTH * TODO_TEXT_HEIGHT / 8)
#define TODO_JSON_BYTES 8192
#define TODO_DUE_BYTES 17
esp_err_t dashboard_todo_get(char *body, size_t size);
esp_err_t dashboard_todo_set(const char *body);
esp_err_t dashboard_todo_render_data(uint8_t *bitmap, unsigned *count, unsigned *done, unsigned *zoom,
    char due[TODO_MAX_ITEMS][TODO_DUE_BYTES]);
esp_err_t dashboard_weather_fetch(int64_t deadline);
bool dashboard_weather_claim_today(void);
bool dashboard_weather_offline(void);
esp_err_t dashboard_weather_set_offline(bool offline);
void dashboard_weather_use_cache(void);
bool dashboard_clock_sync_start(void);
void dashboard_clock_sync_stop(void);
/* Drawing and redraw are called under the screen rotation lock. */
esp_err_t dashboard_show(screen_mode_t mode, unsigned rotation);

#define WEATHER_DAYS 7
#define WEATHER_MISSING (-1000)
typedef struct {
    bool nmc;
    int year, month, day, weekday, hour, minute;
    int temperature, humidity, code, wind, direction, aqi;
    struct { int code, low, high; } daily[WEATHER_DAYS];
} weather_data_t;
const weather_data_t *dashboard_weather_data(void);
const uint8_t *dashboard_weather_name(void);
const char *dashboard_default_location(void);
bool dashboard_weather_ready(void);
bool dashboard_weather_is_live(void);
void dashboard_weather_restore(void);

#endif

