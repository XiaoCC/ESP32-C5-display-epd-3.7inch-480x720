#ifndef SCREEN_ROTATION_H
#define SCREEN_ROTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SCREEN_CACHE_OTP6, SCREEN_CACHE_CQ64, SCREEN_CACHE_CQ256,
    SCREEN_CACHE_CQ256_V2, SCREEN_CACHE_OTP384, SCREEN_CACHE_FAST64,
    SCREEN_CACHE_512_QUAL, SCREEN_CACHE_512_FAST,
} screen_cache_kind_t;

typedef enum {
    SCREEN_MODE_TEXT, SCREEN_MODE_PHOTO, SCREEN_MODE_CALENDAR, SCREEN_MODE_WEATHER,
    SCREEN_MODE_QUOTE, SCREEN_MODE_TODO, SCREEN_MODE_TRIP, SCREEN_MODE_HISTORY, SCREEN_MODE_COUNT,
} screen_mode_t;

/* Lock order: rotation -> display -> board SPI. Never take rotation from a
 * display read/progress callback. HTTP and local UI own this lock per job. */
esp_err_t screen_rotation_init(void);
void screen_rotation_lock(void);
void screen_rotation_unlock(void);
unsigned screen_rotation_get(void);
uint32_t screen_rotation_epoch(void);
bool screen_rotation_busy(void);
void screen_rotation_local_busy(bool busy);
bool screen_rotation_matches(uint32_t epoch);
void screen_rotation_size(unsigned rotation, int *width, int *height);
void screen_rotation_source_xy(unsigned rotation, int px, int py, int *x, int *y);
void screen_rotation_panel_xy(unsigned rotation, int x, int y, int *px, int *py);

/* Call with rotation lock held. The cache stages uploads in an unused flash slot. */
esp_err_t screen_rotation_capture_begin(screen_cache_kind_t kind, const uint8_t blank[3], screen_mode_t mode);
uint32_t screen_rotation_source_generation(void);
esp_err_t screen_rotation_window_begin(uint16_t x, uint16_t y, uint16_t width,
    uint16_t height, uint32_t display_generation, const uint8_t blank[3]);
esp_err_t screen_rotation_capture_write(size_t offset, const uint8_t *data, size_t length);
esp_err_t screen_rotation_capture_end(bool success, uint32_t display_generation, bool save_to_album);
void screen_rotation_invalidate(void);
void screen_rotation_note_ui(void);
void screen_rotation_note_external(void);
void screen_rotation_note_blank(void);
bool screen_rotation_hidden(void);
esp_err_t screen_rotation_toggle_blank(void);
screen_mode_t screen_rotation_next_mode(uint8_t enabled);
screen_mode_t screen_rotation_current_mode(void);
uint8_t screen_rotation_available_modes(void);
unsigned screen_rotation_render_orientation(void);
esp_err_t screen_rotation_redraw_current(void);
esp_err_t screen_rotation_show_mode(screen_mode_t mode);
bool screen_rotation_shortcut_active(void);
esp_err_t screen_rotation_toggle_shortcut(screen_mode_t page, screen_mode_t fallback);

typedef esp_err_t (*screen_album_read_t)(void *context, uint8_t *buffer, size_t length);
bool screen_album_id_valid(const char *name);
size_t screen_album_list(size_t skip, char names[][13], size_t limit);
esp_err_t screen_album_upload(screen_album_read_t read, void *context, uint32_t expected, size_t original_size, unsigned rotation, char saved[13]);
esp_err_t screen_album_thumbnail(const char *name, uint8_t *pixels);
esp_err_t screen_album_show(const char *name);
typedef esp_err_t (*screen_album_send_t)(void *context, unsigned rotation, const uint8_t *data, size_t length);
esp_err_t screen_album_read(const char *name, screen_album_send_t send, void *context, bool original);
size_t screen_album_remove(const char *const *names, size_t count, bool *removed);

#endif
