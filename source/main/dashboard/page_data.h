#ifndef PAGE_DATA_H
#define PAGE_DATA_H

#include "dashboard.h"

#define TRIPS_BYTES 6144
#define LUNAR_BYTES (64 * 1024)

typedef struct {
    char kind[8], date[11], arrival_date[11], depart[6], arrive[6], number[16];
    char from[49], to[49], city[49], seat[49], gate[33], note[73];
    double latitude, longitude;
} page_trip_t;

typedef struct {
    int high, low, rain, code;
    char updated[11];
} page_trip_weather_t;

typedef struct {
    char lunar[37], yi[64], ji[64], term[28];
} page_lunar_t;

typedef struct {
    char date[11], text[273];
    uint32_t fetched_at;
    bool domestic_source;
    int year;
} page_history_t;

esp_err_t page_data_init(void);
esp_err_t page_sd_read(const char *name, char *data, size_t capacity);
esp_err_t page_sd_write(const char *name, const char *data);
screen_mode_t page_shortcut_get(void);
esp_err_t page_shortcut_set(screen_mode_t mode);
esp_err_t page_trips_set(const char *body);
char *page_trips_with_status(const char *body);
esp_err_t page_trip_select(const char *id);
bool page_trip_prune(const struct tm *now);
bool page_deadline_valid(const char *value);
bool page_trip_get(const struct tm *now, page_trip_t *trip);
bool page_trip_weather(const page_trip_t *trip, page_trip_weather_t *weather);
bool page_lunar_get(const struct tm *now, page_lunar_t *lunar);
esp_err_t page_lunar_upload(const char *content);
esp_err_t page_history_set_credentials(const char *id, const char *key);
bool page_history_get(const struct tm *now, page_history_t *history);
void page_network_refresh(void);
esp_err_t page_sync_history(struct tm date);
esp_err_t page_sync_trips(const struct tm *date, bool opened);
void page_trip_opened(void);

#endif
