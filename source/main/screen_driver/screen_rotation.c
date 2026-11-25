#include "screen_rotation.h"
#include "pin_map.h"
#include "dashboard.h"
#include "board.h"
#include "esp_random.h"
#include "tf_card.h"
#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "app_display.h"
#include "screen_driver.h"
#include "screen_ui.h"

#include "web_server.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "rotation";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_worker;
static unsigned s_rotation = EINK_ROTATION;
static uint32_t s_epoch = 1, s_pending;
static bool s_working, s_ui_current, s_local_busy;
static const esp_partition_t *s_partition;

#define CACHE_SLOT_BYTES (256U * 1024U)
#define CACHE_SLOTS 4U
#define SAVED_MODE_COUNT 2U
#define CACHE_SECTOR_BYTES 4096U
#define CACHE_PATCH_LIMIT 16U
typedef struct {
    uint16_t source, gate, sources, gates;
    size_t offset, bytes;
} cache_patch_t;
typedef enum { CAP_NONE, CAP_FULL, CAP_PATCH, CAP_RMW } capture_mode_t;
typedef struct {
    screen_cache_kind_t kind;
    unsigned rotation, planes;
    uint8_t blank[3];
    size_t base, bytes, tail;
    uint32_t generation, source_generation;
    bool valid;

    unsigned patch_count;
    capture_mode_t capturing;
    size_t written, capture_bytes;
    cache_patch_t pending;
    uint8_t *scratch;
    size_t scratch_sector;
    bool scratch_dirty;
} cache_t;
static cache_t s_cache;
typedef struct {
    uint32_t slot;
    uint32_t rotation;
} saved_frame_t;
static saved_frame_t s_saved[SAVED_MODE_COUNT];
static nvs_handle_t s_frame_store;
static bool s_frame_store_open;
static screen_mode_t s_mode = SCREEN_MODE_COUNT;
static screen_mode_t s_shortcut_return_mode = SCREEN_MODE_COUNT;
static cache_t s_shortcut_return_cache;
static screen_mode_t s_shortcut_page = SCREEN_MODE_COUNT;
static screen_mode_t s_capture_mode;
static uint8_t s_saved_mask;
static const char *const mode_keys[] = {"text", "photo"};

static void cancel_shortcut(void)
{
    __atomic_store_n(&s_shortcut_return_mode, SCREEN_MODE_COUNT, __ATOMIC_RELEASE);
    s_shortcut_return_cache.valid = false;
    __atomic_store_n(&s_shortcut_page, SCREEN_MODE_COUNT, __ATOMIC_RELEASE);
}

static uint32_t free_slot(void)
{
    for (uint32_t slot = 0; slot < CACHE_SLOTS; ++slot) {
        bool used = s_cache.valid && s_cache.base == slot * CACHE_SLOT_BYTES;
        used |= s_shortcut_return_cache.valid && s_shortcut_return_cache.base == slot * CACHE_SLOT_BYTES;
        for (unsigned i = 0; i < SAVED_MODE_COUNT; ++i) {
            used |= s_saved[i].slot == slot;
        }
        if (!used) return slot;
    }
    return CACHE_SLOTS;
}
static bool s_blank_current;
static bool s_hidden;
static uint32_t s_hidden_generation;
static uint32_t s_source_generation;

#define ALBUM_DIR TF_CARD_MOUNT_POINT "/photos"
#define ALBUM_MAGIC 0x364b4e49u
typedef struct { uint32_t magic, rotation, hash; } album_header_t;
static char s_album_last[13];
static bool s_album_available;

static uint32_t album_hash(uint32_t hash, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}

/* Hold the board SPI lock around all FAT operations, never during display IO. */
bool screen_album_id_valid(const char *name)
{
    return name && strlen(name) == 12 && !strcasecmp(name + 8, ".E6P") &&
        strspn(name, "0123456789abcdefABCDEF") == 8;
}

size_t screen_album_list(size_t skip, char names[][13], size_t limit)
{
    size_t count = 0;
    board_spi_lock();
    DIR *dir = opendir(ALBUM_DIR);
    if (dir) {
        struct dirent *item;
        while (count < limit && (item = readdir(dir))) {
            if (!screen_album_id_valid(item->d_name)) continue;
            if (skip) { --skip; continue; }
            memcpy(names[count++], item->d_name, 13);
        }
        closedir(dir);
    }
    board_spi_unlock();
    return count;
}

size_t screen_album_remove(const char *const *names, size_t count, bool *removed)
{
    size_t deleted = 0;
    board_spi_lock();
    for (size_t i = 0; i < count; ++i) {
        removed[i] = false;
        if (!screen_album_id_valid(names[i])) continue;
        char path[48];
        snprintf(path, sizeof(path), ALBUM_DIR "/%s", names[i]);
        removed[i] = unlink(path) == 0 || errno == ENOENT;
        if (removed[i]) {
            snprintf(path, sizeof(path), ALBUM_DIR "/%.8s.ORG", names[i]);
            removed[i] = unlink(path) == 0 || errno == ENOENT;
        }
        if (removed[i]) ++deleted;
    }
    board_spi_unlock();

    char first[1][13];
    __atomic_store_n(&s_album_available, screen_album_list(0, first, 1) != 0, __ATOMIC_RELEASE);
    return deleted;
}

static unsigned album_choose(char chosen[13])
{
    DIR *dir = opendir(ALBUM_DIR);
    if (!dir) return 0;
    unsigned count = 0, alternatives = 0;
    struct dirent *item;
    while ((item = readdir(dir))) {
        const char *name = item->d_name;
        if (!screen_album_id_valid(name)) continue;
        ++count;
        if (strcasecmp(name, s_album_last) && esp_random() % ++alternatives == 0)
            memcpy(chosen, name, 13);
    }
    closedir(dir);
    if (!alternatives && count) memcpy(chosen, s_album_last, 13);
    return count;
}

static esp_err_t album_store(size_t base, unsigned rotation, screen_album_read_t read,
    void *context, uint32_t expected, size_t original_size, char saved[13])
{
    if (!tf_card_is_mounted()) return ESP_ERR_NOT_FOUND;
    uint8_t data[1024];
    album_header_t header = {.magic = ALBUM_MAGIC, .rotation = rotation, .hash = 2166136261u};
    char path[48], temporary[48], name[13], original[48], original_tmp[48];
    esp_err_t err = ESP_FAIL;
    board_spi_lock();
    if (mkdir(ALBUM_DIR, 0777) && errno != EEXIST) goto unlock;
    snprintf(name, sizeof(name), "%08lX.E6P", (unsigned long)esp_random());
    snprintf(path, sizeof(path), ALBUM_DIR "/%s", name);
    snprintf(temporary, sizeof(temporary), ALBUM_DIR "/%.8s.TMP", name);
    snprintf(original, sizeof(original), ALBUM_DIR "/%.8s.ORG", name);
    snprintf(original_tmp, sizeof(original_tmp), ALBUM_DIR "/%.8s.TOR", name);
    struct stat st;
    if (!stat(path, &st) || !stat(temporary, &st) ||
        !stat(original, &st) || !stat(original_tmp, &st)) goto unlock;
    FILE *file = fopen(temporary, "wb");
    if (!file) goto unlock;
    bool ok = fwrite(&header, sizeof(header), 1, file) == 1;
    for (size_t offset = 0; ok && offset < SCREEN_FRAME_BYTES; offset += sizeof(data)) {
        const size_t size = SCREEN_FRAME_BYTES - offset < sizeof(data)
            ? SCREEN_FRAME_BYTES - offset : sizeof(data);
        const esp_err_t read_err = read ? read(context, data, size)
            : esp_partition_read(s_partition, base + offset, data, size);
        ok = read_err == ESP_OK && fwrite(data, 1, size, file) == size;
        if (ok) header.hash = album_hash(header.hash, data, size);
    }
    if (ok && read && header.hash != expected) {
        ok = false;
        err = ESP_ERR_INVALID_CRC;
    }
    if (ok) ok = !fseek(file, 0, SEEK_SET) && fwrite(&header, sizeof(header), 1, file) == 1 &&
        !fflush(file) && !fsync(fileno(file));
    if (fclose(file)) ok = false;
    bool original_written = false;
    if (ok && original_size) {
        file = fopen(original_tmp, "wb");
        ok = file != NULL;
        for (size_t left = original_size; ok && left > 0;) {
            const size_t size = left < sizeof(data) ? left : sizeof(data);
            ok = read(context, data, size) == ESP_OK && fwrite(data, 1, size, file) == size;
            left -= size;
        }
        if (file) {
            if (ok) ok = !fflush(file) && !fsync(fileno(file));
            if (fclose(file)) ok = false;
        }
        if (ok) ok = original_written = rename(original_tmp, original) == 0;
        if (!ok) unlink(original_tmp);
    }
    if (ok && !rename(temporary, path)) {
        __atomic_store_n(&s_album_available, true, __ATOMIC_RELEASE);
        if (saved) memcpy(saved, name, 13);
        else memcpy(s_album_last, name, sizeof(s_album_last));
        err = ESP_OK;
    } else {
        unlink(temporary);
        if (original_written) unlink(original);
    }
unlock:
    board_spi_unlock();
    return err;
}

esp_err_t screen_album_upload(screen_album_read_t read, void *context, uint32_t expected, size_t original_size, unsigned rotation, char saved[13])
{
    return album_store(0, rotation, read, context, expected, original_size, saved);
}

esp_err_t screen_album_thumbnail(const char *name, uint8_t *pixels)
{
    if (!screen_album_id_valid(name)) return ESP_ERR_INVALID_ARG;
    char path[48];
    snprintf(path, sizeof(path), ALBUM_DIR "/%s", name);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    board_spi_lock();
    FILE *file = fopen(path, "rb");
    if (file) {
        album_header_t header;
        uint8_t column[SCREEN_HEIGHT / 2];
        bool ok = fread(&header, sizeof(header), 1, file) == 1 && header.magic == ALBUM_MAGIC;
        for (int x = 0; ok && x < 48; ++x) {
            const long offset = sizeof(header) + (SCREEN_WIDTH - 1 - (x * 10 + 5)) * sizeof(column);
            ok = !fseek(file, offset, SEEK_SET) && fread(column, sizeof(column), 1, file) == 1;
            if (ok) for (int y = 0; y < 72; ++y) pixels[y * 48 + x] = column[y * 5 + 2] >> 4;
        }
        fclose(file);
        err = ok ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    board_spi_unlock();
    return err;
}

esp_err_t screen_album_read(const char *name, screen_album_send_t send, void *context, bool original)
{
    if (!screen_album_id_valid(name) || !send) return ESP_ERR_INVALID_ARG;
    char path[48];
    if (original) snprintf(path, sizeof(path), ALBUM_DIR "/%.8s.ORG", name);
    else snprintf(path, sizeof(path), ALBUM_DIR "/%s", name);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    board_spi_lock();
    FILE *file = fopen(path, "rb");
    if (file) {
        album_header_t header = {0};
        uint8_t buffer[2048];
        struct stat info;
        if (fstat(fileno(file), &info) == 0 && (original ? info.st_size > 0 :
                info.st_size == sizeof(header) + SCREEN_FRAME_BYTES &&
                fread(&header, sizeof(header), 1, file) == 1 && header.magic == ALBUM_MAGIC &&
                header.rotation < 360 && header.rotation % 90 == 0)) {
            err = ESP_OK;
            for (size_t remaining = original ? (size_t)info.st_size : SCREEN_FRAME_BYTES; remaining > 0 && err == ESP_OK;) {
                const size_t length = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
                if (fread(buffer, 1, length, file) != length) { err = ESP_FAIL; break; }
                err = send(context, header.rotation, buffer, length);
                remaining -= length;
            }
        }
        fclose(file);
    }
    board_spi_unlock();
    return err;
}

static esp_err_t album_load(size_t base, unsigned *rotation, char chosen[13], const char *requested)
{
    uint8_t data[1024];
    esp_err_t err = ESP_ERR_NOT_FOUND;
    board_spi_lock();
    if (requested) memcpy(chosen, requested, 13);
    else if (!album_choose(chosen)) goto unlock;
    char path[48];
    snprintf(path, sizeof(path), ALBUM_DIR "/%s", chosen);
    FILE *file = fopen(path, "rb");
    if (!file) goto unlock;
    album_header_t header;
    uint32_t hash = 2166136261u;
    bool ok = fread(&header, sizeof(header), 1, file) == 1 && header.magic == ALBUM_MAGIC &&
        header.rotation < 360 && header.rotation % 90 == 0;
    for (size_t offset = 0; ok && offset < SCREEN_FRAME_BYTES; offset += sizeof(data)) {
        const size_t size = SCREEN_FRAME_BYTES - offset < sizeof(data)
            ? SCREEN_FRAME_BYTES - offset : sizeof(data);
        ok = fread(data, 1, size, file) == size &&
            esp_partition_write(s_partition, base + offset, data, size) == ESP_OK;
        if (ok) hash = album_hash(hash, data, size);
    }
    ok = ok && hash == header.hash && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (ok) {
        *rotation = header.rotation;
        err = ESP_OK;
    } else {
        err = ESP_ERR_INVALID_CRC;
    }
unlock:
    board_spi_unlock();
    return err;
}

static size_t sector_size(size_t n) { return (n + CACHE_SECTOR_BYTES - 1U) & ~(CACHE_SECTOR_BYTES - 1U); }

unsigned screen_rotation_get(void) { return __atomic_load_n(&s_rotation, __ATOMIC_ACQUIRE); }
uint32_t screen_rotation_epoch(void) { return __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE); }
bool screen_rotation_busy(void)
{
    return __atomic_load_n(&s_working, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&s_local_busy, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&s_pending, __ATOMIC_ACQUIRE) != 0;
}
void screen_rotation_local_busy(bool busy)
{
    __atomic_store_n(&s_local_busy, busy, __ATOMIC_RELEASE);
}

bool screen_rotation_matches(uint32_t epoch) { return epoch == screen_rotation_epoch(); }
void screen_rotation_lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void screen_rotation_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }
void screen_rotation_size(unsigned r, int *w, int *h)
{
    *w = r == 90 || r == 270 ? SCREEN_HEIGHT : SCREEN_WIDTH;
    *h = r == 90 || r == 270 ? SCREEN_WIDTH : SCREEN_HEIGHT;
}
void screen_rotation_source_xy(unsigned r, int px, int py, int *x, int *y)
{
    switch (r) {
    case 90: *x = py; *y = SCREEN_WIDTH - 1 - px; break;
    case 180: *x = SCREEN_WIDTH - 1 - px; *y = SCREEN_HEIGHT - 1 - py; break;
    case 270: *x = SCREEN_HEIGHT - 1 - py; *y = px; break;
    default: *x = px; *y = py; break;
    }
}
void screen_rotation_panel_xy(unsigned r, int x, int y, int *px, int *py)
{
    switch (r) {
    case 90: *px = SCREEN_WIDTH - 1 - y; *py = x; break;
    case 180: *px = SCREEN_WIDTH - 1 - x; *py = SCREEN_HEIGHT - 1 - y; break;
    case 270: *px = y; *py = SCREEN_HEIGHT - 1 - x; break;
    default: *px = x; *py = y; break;
    }
}
uint32_t screen_rotation_source_generation(void)
{
    return __atomic_load_n(&s_source_generation, __ATOMIC_ACQUIRE);
}
void screen_rotation_invalidate(void)
{
    cancel_shortcut();
    s_hidden = false;
    __atomic_store_n(&s_mode, SCREEN_MODE_COUNT, __ATOMIC_RELEASE);
    s_cache.valid = false; s_cache.capturing = CAP_NONE;
    __atomic_store_n(&s_source_generation, 0, __ATOMIC_RELEASE);
    free(s_cache.scratch); s_cache.scratch = NULL; s_cache.scratch_dirty = false;
}
void screen_rotation_note_external(void)
{
    screen_rotation_invalidate(); s_ui_current = false; s_blank_current = false;
}
void screen_rotation_note_ui(void)
{
    screen_rotation_invalidate(); s_ui_current = true; s_blank_current = false;
}
void screen_rotation_note_blank(void)
{
    screen_rotation_invalidate(); s_ui_current = false; s_blank_current = true;
}

esp_err_t screen_rotation_capture_begin(screen_cache_kind_t kind, const uint8_t blank[3], screen_mode_t mode)
{
    screen_rotation_note_external();
    if (!s_partition) return ESP_ERR_NOT_FOUND;
    if (!blank || mode >= SAVED_MODE_COUNT) return ESP_ERR_INVALID_ARG;
    s_capture_mode = mode;
    unsigned planes = kind == SCREEN_CACHE_OTP6 ? 1 :
        kind == SCREEN_CACHE_CQ64 || kind == SCREEN_CACHE_FAST64 ? 2 : 3;
    const size_t bytes = planes * SCREEN_FRAME_BYTES;
    const size_t erase = sector_size(bytes);
    if (erase > CACHE_SLOT_BYTES) return ESP_ERR_INVALID_SIZE;
    const uint32_t slot = free_slot();
    if (slot >= CACHE_SLOTS) return ESP_ERR_INVALID_STATE;
    const size_t base = slot * CACHE_SLOT_BYTES;
    esp_err_t err = esp_partition_erase_range(s_partition, base, erase);
    if (err != ESP_OK) return err;
    s_cache = (cache_t){.kind = kind, .rotation = screen_rotation_get(),
        .planes = planes, .base = base, .bytes = bytes, .tail = erase,
        .capturing = CAP_FULL, .capture_bytes = bytes};
    memcpy(s_cache.blank, blank, sizeof(s_cache.blank));
    return ESP_OK;
}

esp_err_t screen_rotation_capture_write(size_t offset, const uint8_t *data, size_t length)
{
    if (s_cache.capturing == CAP_NONE) return ESP_ERR_INVALID_STATE;
    if (offset >= s_cache.capture_bytes) return ESP_OK;
    if (offset != s_cache.written || !data) { screen_rotation_invalidate(); return ESP_ERR_INVALID_SIZE; }
    if (length > s_cache.capture_bytes - offset) length = s_cache.capture_bytes - offset;
    esp_err_t err;
    {
        const size_t start = s_cache.capturing == CAP_PATCH ? s_cache.pending.offset : 0;
        err = esp_partition_write(s_partition, s_cache.base + start + offset, data, length);
    }
    if (err != ESP_OK) {
        screen_rotation_invalidate();
        ESP_LOGW(TAG, "image cache write disabled: %s", esp_err_to_name(err));
        return err;
    }
    s_cache.written += length;
    return ESP_OK;
}
esp_err_t screen_rotation_capture_end(bool success, uint32_t generation, bool save_to_album)
{
    bool valid = success && s_cache.capturing != CAP_NONE && s_cache.written == s_cache.capture_bytes;

    free(s_cache.scratch); s_cache.scratch = NULL; s_cache.scratch_dirty = false;
    s_cache.valid = valid; s_cache.capturing = CAP_NONE;
    s_cache.generation = generation; s_cache.source_generation = valid ? generation : 0;
    __atomic_store_n(&s_source_generation, valid ? generation : 0, __ATOMIC_RELEASE);
    if (valid) {
        const saved_frame_t frame = {
            .slot = s_cache.base / CACHE_SLOT_BYTES,
            .rotation = s_cache.rotation,
        };
        esp_err_t err = s_frame_store_open
            ? nvs_set_blob(s_frame_store, mode_keys[s_capture_mode], &frame, sizeof(frame))
            : ESP_OK;
        if (err == ESP_OK && s_frame_store_open) err = nvs_commit(s_frame_store);
        if (err == ESP_OK) {
            s_saved[s_capture_mode] = frame;
            (void)__atomic_or_fetch(&s_saved_mask, 1U << s_capture_mode, __ATOMIC_RELEASE);
            __atomic_store_n(&s_mode, s_capture_mode, __ATOMIC_RELEASE);
        } else {
            ESP_LOGW(TAG, "could not save display mode: %s", esp_err_to_name(err));
        }
    }
    if (!success) return ESP_OK;
    if (!valid) return ESP_FAIL;
    if (save_to_album && s_capture_mode == SCREEN_MODE_PHOTO) return album_store(s_cache.base, s_cache.rotation, NULL, NULL, 0, 0, NULL);
    return ESP_OK;
}

typedef struct {
    const uint8_t *data;
    unsigned target, plane;
    bool mono;
    size_t sent;
    int sw, sh, dw, dh, width, height, left, top;
} replay_t;
static uint8_t replay_pixel(const replay_t *r, int px, int py)
{
    int x, y;
    screen_rotation_source_xy(r->target, px, py, &x, &y);
    if (x < r->left || y < r->top || x >= r->left + r->width || y >= r->top + r->height)
        return s_cache.blank[r->plane];
    const int sx = (x - r->left) * r->sw / r->width;
    const int sy = (y - r->top) * r->sh / r->height;
    screen_rotation_panel_xy(s_cache.rotation, sx, sy, &px, &py);
    const unsigned gate = SCREEN_WIDTH - 1U - px;
    const size_t index = r->plane * SCREEN_FRAME_BYTES +
        gate * (SCREEN_HEIGHT / 2) + py / 2;
    const uint8_t pair = r->data[index];
    return py & 1 ? pair & 15 : pair >> 4;
}
static esp_err_t replay_read(void *context, uint8_t *buffer, size_t capacity, size_t *received)
{
    replay_t *r = context;
    size_t n = SCREEN_FRAME_BYTES - r->sent;
    if (n > capacity) n = capacity;
    for (size_t i = 0; i < n; ++i) {
        const size_t pos = r->sent + i;
        const int px = SCREEN_WIDTH - 1 - (int)(pos / (SCREEN_HEIGHT / 2));
        const int py = (int)(pos % (SCREEN_HEIGHT / 2)) * 2;
        buffer[i] = (uint8_t)((replay_pixel(r, px, py) << 4) | replay_pixel(r, px, py + 1));
        if (r->mono) buffer[i] = screen_driver_mono_pair(buffer[i], pos);
    }
    r->sent += n;
    *received = n;
    return ESP_OK;
}
static void replay_setup(replay_t *r, const uint8_t *mapped, unsigned target)
{
    *r = (replay_t){.data = mapped, .target = target};
    screen_rotation_size(s_cache.rotation, &r->sw, &r->sh);
    screen_rotation_size(target, &r->dw, &r->dh);
    if (r->dw * r->sh <= r->dh * r->sw) { r->width = r->dw; r->height = r->sh * r->dw / r->sw; }
    else { r->height = r->dh; r->width = r->sw * r->dh / r->sh; }
    r->left = (r->dw - r->width) / 2; r->top = (r->dh - r->height) / 2;
}
static esp_err_t replay_progress(void *context, screen_upload_phase_t phase)
{
    (void)context;
    (void)phase;
    return ESP_OK;
}

static esp_err_t replay_image(unsigned target, screen_mode_t mode)
{
    const void *mapped = NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(s_partition, s_cache.base, s_cache.tail,
        ESP_PARTITION_MMAP_DATA, &mapped, &handle);
    if (err != ESP_OK) return err;
    replay_t r;
    replay_setup(&r, mapped, target);
    r.mono = mode < SCREEN_MODE_COUNT && (dashboard_mono_mask() & (1U << mode));
    uint8_t buffer[1024];
    screen_upload_report_t report = {0};
    err = screen_driver_upload(0, buffer, sizeof(buffer), replay_read, &r, replay_progress, &report);
    esp_partition_munmap(handle);
    if (err == ESP_OK) s_cache.generation = report.display_generation;
    else screen_rotation_invalidate();
    return err;
}
screen_mode_t screen_rotation_current_mode(void)
{
    return __atomic_load_n(&s_mode, __ATOMIC_ACQUIRE);
}

uint8_t screen_rotation_available_modes(void)
{
    const unsigned album = __atomic_load_n(&s_album_available, __ATOMIC_ACQUIRE)
        ? (1U << SCREEN_MODE_PHOTO) : 0;
    return album | __atomic_load_n(&s_saved_mask, __ATOMIC_ACQUIRE) |
        (1U << SCREEN_MODE_CALENDAR) | (1U << SCREEN_MODE_WEATHER) |
        (1U << SCREEN_MODE_QUOTE) | (1U << SCREEN_MODE_TODO) |
        (1U << SCREEN_MODE_TRIP) | (1U << SCREEN_MODE_HISTORY);
}

screen_mode_t screen_rotation_next_mode(uint8_t enabled)
{
    enabled &= screen_rotation_available_modes();
    const unsigned start = s_mode == SCREEN_MODE_COUNT ? 0 : (s_mode + 1) % SCREEN_MODE_COUNT;
    for (unsigned step = 0; step < SCREEN_MODE_COUNT; ++step) {
        const screen_mode_t mode = (start + step) % SCREEN_MODE_COUNT;
        if (enabled & (1U << mode)) return mode;
    }
    return SCREEN_MODE_COUNT;
}

static esp_err_t show_mode(screen_mode_t mode, const char *requested)
{
    if (mode >= SCREEN_MODE_COUNT) return ESP_ERR_INVALID_ARG;
    esp_err_t err;
    if (mode >= SAVED_MODE_COUNT) {
        const bool redraw_shortcut = mode == screen_rotation_current_mode() && screen_rotation_shortcut_active();
        err = dashboard_show(mode, screen_rotation_get());
        if (err == ESP_OK && !redraw_shortcut) screen_rotation_note_external();
    } else {
        saved_frame_t frame = s_saved[mode];
        char chosen[13] = {0};
        if (mode == SCREEN_MODE_PHOTO && s_partition &&
                __atomic_load_n(&s_album_available, __ATOMIC_ACQUIRE)) {
            const uint32_t slot = free_slot();
            const size_t base = slot * CACHE_SLOT_BYTES;
            unsigned rotation;
            if (slot < CACHE_SLOTS &&
                esp_partition_erase_range(s_partition, base, sector_size(SCREEN_FRAME_BYTES)) == ESP_OK &&
                album_load(base, &rotation, chosen, requested) == ESP_OK) {
                frame = (saved_frame_t){.slot = slot, .rotation = rotation};
            } else {
                chosen[0] = 0;
            }
        }
        if (requested && !chosen[0]) return ESP_ERR_NOT_FOUND;
        if (!s_partition || frame.slot >= CACHE_SLOTS) return ESP_ERR_NOT_FOUND;
        s_cache = (cache_t){
            .kind = SCREEN_CACHE_OTP6,
            .rotation = frame.rotation,
            .planes = 1,
            .base = frame.slot * CACHE_SLOT_BYTES,
            .bytes = SCREEN_FRAME_BYTES,
            .tail = sector_size(SCREEN_FRAME_BYTES),
            .valid = true,
        };
        err = replay_image(screen_rotation_get(), mode);
        if (err == ESP_OK) {
            if (chosen[0]) memcpy(s_album_last, chosen, sizeof(s_album_last));
            s_cache.source_generation = s_cache.generation;
            __atomic_store_n(&s_source_generation, s_cache.generation, __ATOMIC_RELEASE);
        }
    }
    if (err == ESP_OK) {
        if (mode != __atomic_load_n(&s_shortcut_page, __ATOMIC_ACQUIRE)) cancel_shortcut();
        __atomic_store_n(&s_mode, mode, __ATOMIC_RELEASE);
        s_ui_current = false;
        s_blank_current = false;
        s_hidden = false;
    }
    return err;
}

static unsigned s_render_rotation = 360;

unsigned screen_rotation_render_orientation(void)
{
    return s_render_rotation < 360 ? s_render_rotation : screen_rotation_get();
}

esp_err_t screen_rotation_redraw_current(void)
{
    if (s_mode >= SCREEN_MODE_COUNT) return ESP_ERR_INVALID_STATE;
    if (s_mode >= SAVED_MODE_COUNT) return dashboard_show(s_mode, screen_rotation_get());
    return replay_image(screen_rotation_get(), s_mode);
}

esp_err_t screen_rotation_show_mode(screen_mode_t mode)
{
    return show_mode(mode, NULL);
}

bool screen_rotation_shortcut_active(void)
{
    return screen_rotation_current_mode() == __atomic_load_n(&s_shortcut_page, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&s_shortcut_return_mode, __ATOMIC_ACQUIRE) < SCREEN_MODE_COUNT;
}

esp_err_t screen_rotation_toggle_shortcut(screen_mode_t page, screen_mode_t fallback)
{
    if (!screen_rotation_shortcut_active() && s_mode != page) {
        if (!(screen_rotation_available_modes() & (1U << page))) return ESP_ERR_NOT_FOUND;
        const screen_mode_t origin = s_mode < SCREEN_MODE_COUNT ? s_mode : fallback;
        cache_t origin_cache = {0};
        if (s_mode < SAVED_MODE_COUNT && s_cache.valid && s_cache.capturing == CAP_NONE) {
            origin_cache = s_cache;
            origin_cache.scratch = NULL;
            origin_cache.scratch_dirty = false;
        }
        const esp_err_t err = show_mode(page, NULL);
        if (err == ESP_OK) {
            s_shortcut_return_cache = origin_cache;
            __atomic_store_n(&s_shortcut_page, page, __ATOMIC_RELEASE);
            __atomic_store_n(&s_shortcut_return_mode, origin, __ATOMIC_RELEASE);
        }
        return err;
    }

    const screen_mode_t origin = __atomic_load_n(&s_shortcut_return_mode, __ATOMIC_ACQUIRE);
    const screen_mode_t target = origin < SCREEN_MODE_COUNT ? origin : fallback;
    if (s_shortcut_return_cache.valid) {
        s_cache = s_shortcut_return_cache;
        const esp_err_t err = replay_image(screen_rotation_get(), target);
        if (err == ESP_OK) {
            s_cache.source_generation = s_cache.generation;
            __atomic_store_n(&s_source_generation, s_cache.generation, __ATOMIC_RELEASE);
            __atomic_store_n(&s_mode, target, __ATOMIC_RELEASE);
            s_hidden = false;
            cancel_shortcut();
        } else {
            s_cache.valid = false;
        }
        return err;
    }
    const esp_err_t err = show_mode(target, NULL);
    if (err == ESP_OK) cancel_shortcut();
    return err;
}

esp_err_t screen_album_show(const char *name)
{
    if (!screen_album_id_valid(name)) return ESP_ERR_INVALID_ARG;
    return show_mode(SCREEN_MODE_PHOTO, name);
}

bool screen_rotation_hidden(void)
{
    return s_hidden && s_hidden_generation == screen_driver_generation();
}

esp_err_t screen_rotation_toggle_blank(void)
{
    if (screen_rotation_hidden()) {
        const bool generated = s_mode >= SAVED_MODE_COUNT && s_mode < SCREEN_MODE_COUNT;
        const esp_err_t err = generated ? dashboard_show(s_mode, screen_rotation_get()) :
            s_ui_current ? screen_ui_redraw_rotation(screen_rotation_get()) :
            s_cache.valid && s_cache.generation == s_hidden_generation ? replay_image(screen_rotation_get(), s_mode) :
            ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) s_hidden = false;
        return err;
    }
    s_hidden = false;
    const bool had_image = s_cache.valid && s_cache.generation == screen_driver_generation();
    screen_upload_report_t report = {0};
    esp_err_t err = screen_driver_clear(&report);
    if (err == ESP_OK) {
        s_hidden = true;
        s_hidden_generation = screen_driver_generation();
        if (had_image) s_cache.generation = s_hidden_generation;
    }
    return err;
}

static void rotation_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (__atomic_load_n(&s_pending, __ATOMIC_ACQUIRE)) {
            /* Leave an accepted upload its entire session, including gaps
             * between HTTP requests, before applying queued button presses. */
            if (web_server_screen_busy()) { vTaskDelay(pdMS_TO_TICKS(25)); continue; }
            __atomic_store_n(&s_working, true, __ATOMIC_RELEASE);
            screen_rotation_lock();
            if (web_server_screen_busy()) {
                screen_rotation_unlock();
                __atomic_store_n(&s_working, false, __ATOMIC_RELEASE);
                vTaskDelay(pdMS_TO_TICKS(25)); continue;
            }
            const unsigned steps = __atomic_exchange_n(&s_pending, 0, __ATOMIC_ACQ_REL) & 3U;
            const unsigned target = (screen_rotation_get() + steps * 90U) % 360U;
            ESP_LOGI(TAG, "GPIO28 redraw steps=%u target=%u", steps, target);
            esp_err_t err = ESP_OK;
            if (steps) {
                s_render_rotation = target;
                if (screen_rotation_hidden()) err = ESP_OK;
                else if (s_cache.valid && s_cache.generation == screen_driver_generation()) err = replay_image(target, s_mode);

                else if (s_mode >= SAVED_MODE_COUNT && s_mode < SCREEN_MODE_COUNT) err = dashboard_show(s_mode, target);
                else if (s_ui_current) err = screen_ui_redraw_rotation(target);
                else if (s_blank_current) err = ESP_OK;
                else { err = ESP_ERR_INVALID_STATE; ESP_LOGE(TAG, "no current image cache; flash partition table and upload again"); }
                s_render_rotation = 360;
                if (err == ESP_OK) {
                    __atomic_store_n(&s_rotation, target, __ATOMIC_RELEASE);
                    if (__atomic_add_fetch(&s_epoch, 1, __ATOMIC_ACQ_REL) == 0)
                        (void)__atomic_add_fetch(&s_epoch, 1, __ATOMIC_ACQ_REL);
                }
                ESP_LOGI(TAG, "button rotate target=%u applied=%u epoch=%u err=%s", target,
                    screen_rotation_get(), (unsigned)screen_rotation_epoch(), esp_err_to_name(err));
            }
            screen_rotation_unlock();
            __atomic_store_n(&s_working, false, __ATOMIC_RELEASE);
        }
    }
}
static void rotation_button(void *arg)
{
    (void)arg;
    int raw = gpio_get_level(EINK_ROTATE_BUTTON), stable = raw;
    TickType_t changed = xTaskGetTickCount();
    TickType_t pressed_at = 0, released_at = 0;
    bool armed = raw != EINK_ROTATE_BUTTON_ACTIVE_LEVEL;
    bool down = false, click_pending = false, held = false;
    const TickType_t double_click = pdMS_TO_TICKS(350);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const int value = gpio_get_level(EINK_ROTATE_BUTTON);
        const TickType_t now = xTaskGetTickCount();
        if (value != raw) {
            raw = value;
            changed = now;
        }
        if (raw != stable && now - changed >= pdMS_TO_TICKS(30)) {
            stable = raw;
            if (stable == EINK_ROTATE_BUTTON_ACTIVE_LEVEL && armed) {
                down = true;
                held = false;
                pressed_at = now;
            } else if (stable != EINK_ROTATE_BUTTON_ACTIVE_LEVEL) {
                // A release without a matching press belongs to a held boot strap.
                if (down && !held && now - pressed_at < pdMS_TO_TICKS(850)) {
                    if (click_pending && now - released_at <= double_click) {
                        click_pending = false;
                        (void)__atomic_add_fetch(&s_pending, 1, __ATOMIC_ACQ_REL);
                        xTaskNotifyGive(s_worker);
                    } else {
                        if (click_pending) app_display_next();
                        click_pending = true;
                        released_at = now;
                    }
                }
                down = false;
                armed = true;
            }
        }
        if (down && raw == stable && !held && now - pressed_at >= pdMS_TO_TICKS(850)) {
            held = true;
            click_pending = false;
            app_display_wifi();
        }
        if (click_pending && now - released_at > double_click) {
            click_pending = false;
            app_display_next();
        }
    }
}
esp_err_t screen_rotation_init(void)
{
    if (s_lock) return ESP_OK;
    s_frame_store_open = nvs_open("eink_modes", NVS_READWRITE, &s_frame_store) == ESP_OK;
    unsigned occupied = 0;
    for (unsigned i = 0; i < SAVED_MODE_COUNT; ++i) {
        saved_frame_t frame;
        size_t len = sizeof(frame);
        s_saved[i].slot = CACHE_SLOTS;
        if (s_frame_store_open &&
            nvs_get_blob(s_frame_store, mode_keys[i], &frame, &len) == ESP_OK &&
            len == sizeof(frame) && frame.slot < CACHE_SLOTS && frame.rotation < 360 &&
            frame.rotation % 90 == 0 && !(occupied & (1U << frame.slot))) {
            s_saved[i] = frame;
            (void)__atomic_or_fetch(&s_saved_mask, 1U << i, __ATOMIC_RELEASE);
            occupied |= 1U << frame.slot;
        }
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "image_cache");
    if (!s_partition || s_partition->size < CACHE_SLOTS * CACHE_SLOT_BYTES) {
        s_partition = NULL;
        __atomic_store_n(&s_saved_mask, 0, __ATOMIC_RELEASE);
        ESP_LOGW(TAG, "image_cache partition unavailable; flash the updated partition table");
    }
    if (s_partition && tf_card_is_mounted()) {
        char chosen[13] = {0};
        board_spi_lock();
        const bool available = album_choose(chosen) != 0;
        board_spi_unlock();
        __atomic_store_n(&s_album_available, available, __ATOMIC_RELEASE);
    }
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << EINK_ROTATE_BUTTON, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) { vSemaphoreDelete(s_lock); s_lock = NULL; return err; }
    if (xTaskCreate(rotation_worker, "rotate_ui", 4096, NULL, 3, &s_worker) != pdPASS) {
        vSemaphoreDelete(s_lock); s_lock = NULL; return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(rotation_button, "rotate_key", 2048, NULL, 2, NULL) != pdPASS) {
        vTaskDelete(s_worker); s_worker = NULL; vSemaphoreDelete(s_lock); s_lock = NULL; return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "GPIO%d ready (single: mode; double: rotate), default=%u cache=%s",
        (int)EINK_ROTATE_BUTTON, screen_rotation_get(), s_partition ? "flash" : "unavailable");
    return ESP_OK;
}
