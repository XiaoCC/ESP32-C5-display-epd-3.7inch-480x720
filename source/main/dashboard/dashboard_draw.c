#include "dashboard.h"
#include "page_data.h"
#include "axp2101.h"
#include "glyphs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "screen_driver.h"
#include "tf_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef enum { RECT, CIRCLE, GLYPH, BITMAP, DASH } shape_t;
typedef struct {
    shape_t shape;
    int x, y, w, h, scale;
    uint8_t color;
    const void *bits;
} draw_op_t;

/* Render the display a column at a time to avoid a 172800-byte framebuffer. */
static draw_op_t s_ops[200];
static size_t s_count;
static size_t s_sent;
static unsigned s_rotation;
static bool s_mono;
static int s_last_x, s_width, s_height;
/* Both orientations have at most 720 pixels per streamed line. */
static uint8_t s_column[SCREEN_HEIGHT];
static const char *const weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

#define BLACK SCREEN_COLOR_BLACK
#define BLUE SCREEN_COLOR_BLUE
#define GREEN SCREEN_COLOR_GREEN
#define RED SCREEN_COLOR_RED
#define WHITE SCREEN_COLOR_WHITE
#define YELLOW SCREEN_COLOR_YELLOW

static void add(draw_op_t op)
{
    if (s_count < sizeof(s_ops) / sizeof(s_ops[0])) s_ops[s_count++] = op;
}

static void rect(int x, int y, int w, int h, uint8_t color)
{
    add((draw_op_t){.shape = RECT, .x = x, .y = y, .w = w, .h = h, .color = color});
}

static void circle(int x, int y, int r, uint8_t color)
{
    add((draw_op_t){.shape = CIRCLE, .x = x - r, .y = y - r, .w = r * 2 + 1,
        .h = r * 2 + 1, .scale = r, .color = color});
}

static unsigned next_code(const char **input)
{
    const unsigned char *p = (const unsigned char *)*input;
    unsigned result = *p++;
    if (result >= 0xC2 && result <= 0xDF && (p[0] & 0xC0) == 0x80) {
        result = ((result & 31) << 6) | (*p++ & 63);
    } else if ((result & 0xF0) == 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
        result = ((result & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63);
        p += 2;
        if (result < 0x800) result = 0xFFFD;
    } else if (result >= 128) {
        result = 0xFFFD;
    }
    *input = (const char *)p;
    return result;
}

typedef struct { uint16_t code; uint16_t row[16]; } quote_glyph_t;
extern const quote_glyph_t quote_font_start[] asm("_binary_quote_font_bin_start");
extern const uint8_t quote_font_end[] asm("_binary_quote_font_bin_end");

static const uint16_t *find_glyph(unsigned code)
{
    for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i) {
        if (glyphs[i].code == code) return glyphs[i].row;
    }
    size_t low = 0;
    size_t high = (quote_font_end - (const uint8_t *)quote_font_start) / sizeof(quote_glyph_t);
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        const quote_glyph_t *glyph = &quote_font_start[mid];
        if (glyph->code == code) return glyph->row;
        if (glyph->code < code) low = mid + 1;
        else high = mid;
    }
    return NULL;
}

static int text_width(const char *value)
{
    int width = 0;
    while (*value) width += next_code(&value) < 128 ? 10 : 17;
    return width;
}

static void text(int x, int y, const char *value, int scale, uint8_t color)
{
    while (*value) {
        unsigned code = next_code(&value);
        const int advance = (code < 128 ? 10 : 17) * scale;
        const uint16_t *rows = find_glyph(code);
        if (rows) {
            add((draw_op_t){.shape = GLYPH, .x = x, .y = y, .w = 16 * scale,
                .h = 16 * scale, .scale = scale, .color = color, .bits = rows});
        }
        x += advance;
    }
}

static void centered(int center, int y, const char *value, int scale, uint8_t color)
{
    text(center - text_width(value) * scale / 2, y, value, scale, color);
}

static int month_length(int year, int month)
{
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month] + (month == 1 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static void calendar_layout(void)
{
    struct tm now;
    if (!dashboard_local_time(&now)) {
        centered(240, 309, "等待时间同步", 3, BLUE);
        return;
    }
    char date[40];
    snprintf(date, sizeof(date), "%04d年%02d月", now.tm_year + 1900, now.tm_mon + 1);
    text(26, 31, date, 3, BLUE);
    snprintf(date, sizeof(date), "%02d:%02d", now.tm_hour, now.tm_min);
    text(350, 40, date, 2, GREEN);
    snprintf(date, sizeof(date), "%02d", now.tm_mday);
    centered(240, 125, date, 8, BLUE);
    centered(240, 249, weekdays[now.tm_wday], 2, BLACK);
    rect(23, 301, 434, 2, BLUE);

    const int first = (now.tm_wday - (now.tm_mday - 1) % 7 + 13) % 7;
    const int last = month_length(now.tm_year + 1900, now.tm_mon);
    for (int i = 0; i < 7; ++i) {
        centered(54 + 62 * i, 323, weekdays[(i + 1) % 7] + 3, 2, i >= 5 ? RED : BLACK);
    }
    for (int day = 1; day <= last; ++day) {
        const int index = first + day - 1;
        const int x = 54 + (index % 7) * 62;
        const int y = 376 + (index / 7) * 52;
        snprintf(date, sizeof(date), "%d", day);
        if (day == now.tm_mday) circle(x, y + 14, 24, BLUE);
        centered(x, y, date, 2, day == now.tm_mday ? WHITE : index % 7 >= 5 ? RED : BLACK);
    }
}

static int s_quote_day = -1;
enum { QUOTE_FIRST, QUOTE_SECOND, QUOTE_TITLE, QUOTE_AUTHOR, QUOTE_URL, QUOTE_FIELDS };
static const char *const quotes[][QUOTE_FIELDS] = {
    {"直到这一刻", "我才认清自己", "《傲慢与偏见》", "简·奥斯汀", "https://www.gutenberg.org/ebooks/1342"},
    {"我究竟是谁", "这才是个谜", "《爱丽丝漫游奇境》", "刘易斯·卡罗尔", "https://www.gutenberg.org/ebooks/11"},
    {"我不是鸟", "没有网能困住我", "《简·爱》", "夏洛蒂·勃朗特", "https://www.gutenberg.org/ebooks/1260"},
    {"我不怕风暴", "我正学着掌舵", "《小妇人》", "路易莎·梅·奥尔科特", "https://www.gutenberg.org/ebooks/514"},
    {"那是最好的时代", "也是最坏的时代", "《双城记》", "查尔斯·狄更斯", "https://www.gutenberg.org/ebooks/98"},
    {"生存还是毁灭", "这是一个问题", "《哈姆雷特》", "威廉·莎士比亚", "https://www.gutenberg.org/ebooks/1524"},
    {"除了一个孩子", "所有孩子都会长大", "《彼得·潘》", "詹姆斯·巴里", "https://www.gutenberg.org/ebooks/16"},
    {"有朋自远方来", "不亦乐乎", "《论语·学而》", "孔子及其弟子", "https://ctext.org/analects/xue-er/zhs"},
    {"千里之行", "始于足下", "《道德经》第六十四章", "老子", "https://ctext.org/dao-de-jing/zhs"},
};

#define QUOTE_FILE TF_CARD_MOUNT_POINT "/QUOTES.TSV"
#define QUOTE_TEMP TF_CARD_MOUNT_POINT "/QUOTES.TMP"
#define QUOTE_BACKUP TF_CARD_MOUNT_POINT "/QUOTES.BAK"
#define QUOTE_LIBRARY_BYTES (32 * 1024)
#define QUOTE_LINE_BYTES 512
static SemaphoreHandle_t s_quote_lock;
static char s_quote_preview[QUOTE_LINE_BYTES];

void dashboard_quote_init(void)
{
    s_quote_lock = xSemaphoreCreateMutex();
    if (!tf_card_is_mounted()) return;
    struct stat info;
    if (stat(QUOTE_FILE, &info) == 0 || rename(QUOTE_BACKUP, QUOTE_FILE) == 0) {
        remove(QUOTE_TEMP);
        remove(QUOTE_BACKUP);
        return;
    }
    FILE *file = fopen(QUOTE_TEMP, "w");
    if (!file) return;
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof(quotes) / sizeof(quotes[0]); ++i) {
        const char *const *entry = quotes[i];
        ok = fprintf(file, "%s\t%s\t%s\t%s\t%s\n",
            entry[QUOTE_FIRST], entry[QUOTE_SECOND], entry[QUOTE_TITLE],
            entry[QUOTE_AUTHOR], entry[QUOTE_URL]) > 0;
    }
    if (fclose(file) != 0) ok = false;
    if (ok && rename(QUOTE_TEMP, QUOTE_FILE) == 0) {
        ESP_LOGI("dashboard", "Seeded SD quote database at " QUOTE_FILE);
        return;
    }
    remove(QUOTE_TEMP);
}

static bool quote_text_valid(const char *value)
{
    int width = 0;
    for (const char *p = value; *p;) {
        const unsigned code = next_code(&p);
        if (!find_glyph(code)) return false;
        width += code < 128 ? 10 : 17;
    }
    return width > 0 && width <= 434;
}

static bool parse_quote(char *line, const char **fields)
{
    for (int i = 0; i < QUOTE_FIELDS - 1; ++i) {
        char *tab = strchr(line, '\t');
        if (!tab) return false;
        *tab = 0;
        fields[i] = line;
        if (!quote_text_valid(line)) return false;
        line = tab + 1;
    }
    fields[QUOTE_URL] = line;
    return (!*line || (strncmp(line, "https://", 8) == 0 && line[8])) &&
        !strpbrk(line, "\t\r\n ");
}

static esp_err_t read_quote_library(char **content, size_t *length)
{
    FILE *file = fopen(QUOTE_FILE, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    esp_err_t err = ESP_FAIL;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long size = ftell(file);
        if (size >= 0 && size <= QUOTE_LIBRARY_BYTES && fseek(file, 0, SEEK_SET) == 0) {
            char *buffer = malloc((size_t)size + 1);
            if (!buffer) err = ESP_ERR_NO_MEM;
            else if (fread(buffer, 1, size, file) != (size_t)size) free(buffer);
            else {
                buffer[size] = 0;
                *content = buffer;
                *length = size;
                err = ESP_OK;
            }
        } else if (size > QUOTE_LIBRARY_BYTES) err = ESP_ERR_INVALID_SIZE;
    }
    fclose(file);
    return err;
}

esp_err_t dashboard_quote_export(char **content, size_t *length)
{
    if (!s_quote_lock || !tf_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_quote_lock, portMAX_DELAY);
    const esp_err_t err = read_quote_library(content, length);
    xSemaphoreGive(s_quote_lock);
    return err;
}

static bool quote_already_saved(const char *saved, const char *line)
{
    const size_t length = strlen(line);
    for (const char *hit = strstr(saved, line); hit; hit = strstr(hit + 1, line)) {
        if ((hit == saved || hit[-1] == '\n') &&
            (hit[length] == '\0' || hit[length] == '\n' || hit[length] == '\r')) return true;
    }
    return false;
}

esp_err_t dashboard_quote_import(char *content, unsigned *bad_row)
{
    if (!s_quote_lock || !tf_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_quote_lock, portMAX_DELAY);
    char *saved = NULL;
    size_t length = 0;
    esp_err_t err = read_quote_library(&saved, &length);
    if (err != ESP_OK) goto unlock;
    FILE *file = fopen(QUOTE_TEMP, "w");
    if (!file) { err = ESP_FAIL; goto unlock; }

    bool changed = false;
    if (fwrite(saved, 1, length, file) != length) err = ESP_FAIL;
    if (err == ESP_OK && length && saved[length - 1] != '\n') {
        if (fputc('\n', file) == EOF) err = ESP_FAIL;
        ++length;
    }
    char *cursor = content;
    for (unsigned row = 1; err == ESP_OK && cursor; ++row) {
        char *line = strsep(&cursor, "\n");
        size_t bytes = strlen(line);
        if (bytes && line[bytes - 1] == '\r') line[--bytes] = 0;
        if (!bytes) continue;
        char copy[QUOTE_LINE_BYTES];
        const char *fields[QUOTE_FIELDS];
        if (bytes >= sizeof(copy) || !parse_quote(strcpy(copy, line), fields)) {
            *bad_row = row;
            err = ESP_ERR_INVALID_ARG;
        } else if (!quote_already_saved(saved, line)) {
            length += bytes + 1;
            if (length > QUOTE_LIBRARY_BYTES) err = ESP_ERR_INVALID_SIZE;
            else if (fprintf(file, "%s\n", line) < 0) err = ESP_FAIL;
            else changed = true;
        }
    }
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK && changed) {
        remove(QUOTE_BACKUP);
        if (rename(QUOTE_FILE, QUOTE_BACKUP) != 0) err = ESP_FAIL;
        else if (rename(QUOTE_TEMP, QUOTE_FILE) != 0) {
            rename(QUOTE_BACKUP, QUOTE_FILE);
            err = ESP_FAIL;
        } else remove(QUOTE_BACKUP);
    }
    remove(QUOTE_TEMP);
unlock:
    free(saved);
    xSemaphoreGive(s_quote_lock);
    return err;
}

esp_err_t dashboard_quote_preview(const char *content)
{
    if (!s_quote_lock) return ESP_ERR_INVALID_STATE;
    char copy[QUOTE_LINE_BYTES];
    const char *fields[QUOTE_FIELDS];
    if (strlen(content) >= sizeof(copy) || !parse_quote(strcpy(copy, content), fields)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_quote_lock, portMAX_DELAY);
    strcpy(s_quote_preview, content);
    xSemaphoreGive(s_quote_lock);
    return ESP_OK;
}

static bool sd_quote(size_t day, char *line, const char **fields)
{
    if (!tf_card_is_mounted()) return false;
    FILE *file = fopen(QUOTE_FILE, "r");
    if (!file) return false;
    bool found = false;
    size_t count = 0;
    /* First count valid entries, then select by date. */
    for (int pass = 0; pass < 2 && !found; ++pass) {
        size_t index = 0;
        while (fgets(line, QUOTE_LINE_BYTES, file)) {
            const size_t end = strcspn(line, "\r\n");
            if (!line[end] && !feof(file)) {
                int ch;
                while ((ch = fgetc(file)) != '\n' && ch != EOF) {}
                continue;
            }
            line[end] = 0;
            if (!parse_quote(line, fields)) continue;
            if (pass == 0) ++count;
            else if (index++ == day % count) {
                found = true;
                break;
            }
        }
        if (!count || ferror(file)) break;
        rewind(file);
    }
    fclose(file);
    if (found) ESP_LOGI("dashboard", "Daily quote read from " QUOTE_FILE);
    return found;
}

static int quote_day(struct tm *local)
{
    if (!dashboard_local_time(local)) return -1;
    struct tm noon = *local;
    noon.tm_hour = 12;
    noon.tm_min = 0;
    noon.tm_sec = 0;
    return (int)(mktime(&noon) / 86400);
}

bool dashboard_quote_due(void)
{
    struct tm local;
    const int day = quote_day(&local);
    return day >= 0 && day != __atomic_load_n(&s_quote_day, __ATOMIC_RELAXED);
}

static void quote_layout(const struct tm *local, int day)
{
    rect(28, 33, 5, 51, RED);
    text(48, 32, "日历日签", 2, BLACK);
    if (day >= 0) {
        char date[32];
        strftime(date, sizeof(date), "%Y-%m", local);
        text(333, 42, date, 2, BLUE);
        text(381, 88, weekdays[local->tm_wday], 1, BLUE);
        strftime(date, sizeof(date), "%d", local);
        centered(240, 130, date, 10, BLACK);
        page_lunar_t lunar;
        if (page_lunar_get(local, &lunar)) {
            snprintf(date, sizeof(date), "%s", lunar.term);
            centered(240, 294, lunar.lunar, 2, BLUE);
            if (*date) centered(240, 333, date, 1, GREEN);
            rect(31, 375, 418, 2, BLACK);
            text(34, 397, "宜", 2, GREEN);
            text(82, 401, lunar.yi, 1, BLACK);
            text(34, 442, "忌", 2, RED);
            text(82, 446, lunar.ji, 1, BLACK);
        } else centered(240, 318, "SD 卡暂无本年农历", 1, RED);
    } else centered(240, 252, "等待时间同步", 2, BLUE);
    rect(31, 488, 418, 2, BLUE);
    const size_t date_index = day < 0 ? 0 : (size_t)day;
    const size_t index = date_index % (sizeof(quotes) / sizeof(quotes[0]));
    char saved[QUOTE_LINE_BYTES];
    const char *fields[QUOTE_FIELDS];
    const char *const *entry = quotes[index];
    if (s_quote_lock) {
        xSemaphoreTake(s_quote_lock, portMAX_DELAY);
        if (s_quote_preview[0]) {
            if (parse_quote(strcpy(saved, s_quote_preview), fields)) entry = fields;
            s_quote_preview[0] = 0;
            ESP_LOGI("dashboard", "Selected quote displayed");
        } else if (sd_quote(date_index, saved, fields)) entry = fields;
        xSemaphoreGive(s_quote_lock);
    }
    const int first_width = text_width(entry[QUOTE_FIRST]);
    const int second_width = text_width(entry[QUOTE_SECOND]);
    const int width = first_width > second_width ? first_width : second_width;
    const int scale = width <= 144 ? 3 : width <= 217 ? 2 : 1;
    const int quote_scale = scale > 2 ? 2 : scale;
    centered(240, 509, entry[QUOTE_FIRST], quote_scale, BLACK);
    centered(240, 550, entry[QUOTE_SECOND], quote_scale, BLACK);
    centered(240, 610, entry[QUOTE_TITLE], 1, BLUE);
    centered(240, 638, entry[QUOTE_AUTHOR], 1, BLACK);
}

static uint8_t s_todo_bitmap[TODO_MAX_ITEMS * TODO_ROW_BYTES];

static void todo_layout(void)
{
    unsigned count, done, zoom;
    char due[TODO_MAX_ITEMS][TODO_DUE_BYTES] = {0};
    const esp_err_t err = dashboard_todo_render_data(s_todo_bitmap, &count, &done, &zoom, due);
    unsigned completed = 0;
    for (unsigned i = 0; i < count; ++i) completed += (done >> i) & 1U;
    char progress[24];
    snprintf(progress, sizeof(progress), "%u / %u", completed, count);
    rect(28, 23, 5, 33, BLUE);
    text(45, 23, "待办事项", 2, BLACK);
    centered(394, 32, progress, 2, BLUE);
    struct tm now;
    if (dashboard_local_time(&now)) {
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%H:%M", &now);
        text(28, 64, stamp, 4, BLACK);
        snprintf(stamp, sizeof(stamp), "%02d/%02d %s", now.tm_mon + 1, now.tm_mday, weekdays[now.tm_wday]);
        text(318, 104, stamp, 1, BLUE);
    }
    rect(28, 150, 424, 4, BLACK);
    if (count) rect(28, 150, 424 * completed / count, 4, GREEN);
    if (err != ESP_OK || !count) {
        centered(240, 316, err == ESP_OK ? "暂无待办" : "读取失败", 3, BLACK);
        centered(240, 387, "请在网页管理", 2, BLUE);
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        const int y = 186 + i * 91;
        const bool checked = (done & (1U << i)) != 0;
        const uint8_t color = checked ? GREEN : BLUE;
        rect(34, y + 18, 28, 28, color);
        if (checked) {
            for (int j = 0; j < 6; ++j) rect(40 + j, y + 31 + j, 3, 3, WHITE);
            for (int j = 0; j < 12; ++j) rect(45 + j, y + 36 - j, 3, 3, WHITE);
        } else rect(37, y + 21, 22, 22, WHITE);
        const int text_height = TODO_TEXT_HEIGHT * zoom / 100;
        add((draw_op_t){.shape = BITMAP, .x = 85, .y = y + 32 - text_height / 2,
            .w = due[i][0] ? 237 : TODO_TEXT_WIDTH, .h = text_height, .scale = zoom,
            .color = checked ? GREEN : BLACK, .bits = s_todo_bitmap + i * TODO_ROW_BYTES});
        if (due[i][0]) {
            char deadline[14];
            snprintf(deadline, sizeof(deadline), "%.5s %.5s", due[i] + 5, due[i] + 11);
            text(335, y + 24, deadline, 1, BLUE);
        }
        add((draw_op_t){.shape = DASH, .x = 28, .y = y + 70, .w = 424, .h = 1, .color = BLACK});
    }
}

static const char *weather_label(int code)
{
    if (code == 0) return "晴";
    if (code == 1 || code == 2) return "多云";
    if (code == 3) return "阴";
    if (code == 45 || code == 48) return "雾";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "雪";
    if (code >= 95) return "雷雨";
    if (code >= 51) return "雨";
    return "-";
}

static void weather_icon(int x, int y, int r, int code)
{
    if (code == WEATHER_MISSING) {
        rect(x - r / 3, y, r * 2 / 3, 2, BLUE);
        return;
    }
    if (code == 0) {
        circle(x, y, r * 3 / 5, YELLOW);
        const int t = r / 8 + 1, d = r * 3 / 4;
        rect(x - t / 2, y - r, t, r - d, YELLOW);
        rect(x - t / 2, y + d, t, r - d, YELLOW);
        rect(x - r, y - t / 2, r - d, t, YELLOW);
        rect(x + d, y - t / 2, r - d, t, YELLOW);
    } else {
        circle(x - r / 2, y, r / 3, BLUE);
        circle(x, y - r / 4, r / 2, BLUE);
        circle(x + r / 2, y, r / 3, BLUE);
        rect(x - r / 2, y, r, r / 3, BLUE);
        if (code >= 51) {
            for (int i = -1; i <= 1; ++i) {
                rect(x + i * (r / 2), y + r / 2, r / 8 + 1, r / 5 + 1, BLUE);
            }
        }
    }
}

static void reading(char *out, size_t size, int value, const char *suffix)
{
    if (value == WEATHER_MISSING) snprintf(out, size, "-");
    else snprintf(out, size, "%d%s", value, suffix);
}

static void weather_layout(void)
{
    const weather_data_t *w = dashboard_weather_data();
    struct tm local;
    const bool clock_valid = dashboard_local_time(&local);
    char line[40], a[14], b[14];
    if (clock_valid) {
        snprintf(line, sizeof(line), "%02d:%02d", local.tm_hour, local.tm_min);
        text(24, 66, line, 2, GREEN);
    }
    if (!dashboard_weather_ready()) {
        centered(240, 279, "天气时钟", 3, BLUE);
        centered(240, 359, "天气数据未更新", 2, BLACK);
        centered(240, 407, "等待联网更新", 2, BLACK);
        return;
    }
    const uint8_t *name = dashboard_weather_name();
    if (name) {
        add((draw_op_t){.shape = BITMAP, .x = 24, .y = 25, .w = WEATHER_NAME_WIDTH,
            .h = WEATHER_NAME_HEIGHT, .color = BLACK, .bits = name});
    } else {
        text(24, 25, "扬州", 2, BLACK);
    }
    snprintf(line, sizeof(line), "%04d-%02d-%02d", w->year, w->month, w->day);
    text(252, 24, line, 2, BLUE);
    snprintf(line, sizeof(line), "%s  数据 %02d:%02d", weekdays[w->weekday], w->hour, w->minute);
    text(252, 72, line, 1, GREEN);
    if (!dashboard_weather_is_live()) text(24, 687, "CACHE", 1, BLUE);
    const bool is_today = clock_valid && local.tm_year + 1900 == w->year &&
        local.tm_mon + 1 == w->month && local.tm_mday == w->day;

    weather_icon(105, 214, 67, w->code);
    reading(line, sizeof(line), w->temperature, "");
    centered(283, 136, line, 8, BLUE);
    text(403, 162, "C", 3, BLUE);
    circle(396, 171, 5, BLUE);
    circle(396, 171, 3, WHITE);
    centered(279, 277, weather_label(w->code), 2, BLACK);
    reading(a, sizeof(a), w->daily[0].low, "");
    reading(b, sizeof(b), w->daily[0].high, "");
    snprintf(line, sizeof(line), "%s / %s C", a, b);
    centered(279, 322, line, 2, BLACK);

    rect(24, 372, 432, 2, BLUE);
    rect(168, 394, 2, 83, BLUE);
    rect(312, 394, 2, 83, BLUE);
    centered(96, 397, "湿度", 2, BLACK);
    reading(line, sizeof(line), w->humidity, "%");
    centered(96, 441, line, 2, GREEN);
    static const char *directions[] = {"北风","东北风","东风","东南风","南风","西南风","西风","西北风"};
    centered(240, 397, w->direction < 0 || w->direction > 360 ? "风速" :
        directions[((w->direction + 22) / 45) % 8], 2, BLACK);
    reading(line, sizeof(line), w->wind, " M/S");
    centered(240, 441, line, 2, GREEN);
    centered(384, 397, w->nmc ? "空气质量" : "US AQI", 2, BLACK);
    reading(line, sizeof(line), w->aqi, "");
    centered(384, 441, line, 2, GREEN);
    rect(24, 503, 432, 2, BLUE);

    for (int i = 0; i < WEATHER_DAYS; ++i) {
        const int x = 48 + i * 64;
        centered(x, 522, i == 0 && is_today ? "今天" : weekdays[(w->weekday + i) % 7], 1, BLACK);
        weather_icon(x, 572, 18, w->daily[i].code);
        reading(a, sizeof(a), w->daily[i].low, "");
        reading(b, sizeof(b), w->daily[i].high, "");
        snprintf(line, sizeof(line), "%s/%s", a, b);
        centered(x, 604, line, 1, BLACK);
        centered(x, 634, weather_label(w->daily[i].code), 1, RED);
    }
    text(247, 687, w->nmc ? "中央气象台" : "OPEN-METEO / CAMS", 1, BLACK);
}

static void clipped_text(int x, int y, const char *value, int scale, int width, uint8_t color)
{
    char line[224];
    const char *p = value, *end = value;
    int used = 0;
    while (*p) {
        used += (next_code(&p) < 128 ? 10 : 17) * scale;
        if (used > width || p - value >= sizeof(line)) break;
        end = p;
    }
    memcpy(line, value, end - value);
    line[end - value] = 0;
    text(x, y, line, scale, color);
}

static void trip_layout(void)
{
    struct tm now;
    const bool clock = dashboard_local_time(&now);
    page_trip_t trip;
    rect(28, 36, 5, 46, BLUE);
    text(47, 39, "我的行程", 2, BLACK);
    if (!page_trip_get(clock ? &now : NULL, &trip)) {
        centered(240, 305, "暂无待出发行程", 2, BLACK);
        centered(240, 355, "请在后台添加机票或高铁票", 1, BLUE);
        return;
    }
    const bool flight = !strcmp(trip.kind, "flight");
    char line[128];
    snprintf(line, sizeof(line), "%s  %s", flight ? "航班" : "车次", trip.number);
    text(30, 122, line, 2, BLUE);
    text(322, 130, trip.date, 1, BLACK);
    rect(28, 168, 424, 2, BLACK);
    text(28, 195, "出发", 1, BLUE);
    text(268, 195, "到达", 1, BLUE);
    text(28, 224, trip.depart, 4, BLACK);
    text(268, 224, trip.arrive, 4, BLACK);
    // A filled shaft and a stepped head stay legible on the six-color panel.
    rect(229, 253, 27, 5, BLUE);
    for (int column = 0; column < 13; ++column) {
        rect(249 + column, 243 + column, 1, 25 - column * 2, BLUE);
    }
    clipped_text(28, 305, trip.from, 2, 202, BLACK);
    clipped_text(268, 305, trip.to, 2, 184, BLACK);
    if (strcmp(trip.arrival_date, trip.date)) text(268, 347, trip.arrival_date, 1, BLUE);
    snprintf(line, sizeof(line), "%s %s", flight ? "登机口" : "检票口", trip.gate[0] ? trip.gate : "-");
    clipped_text(28, 388, line, 2, 211, BLACK);
    snprintf(line, sizeof(line), "座位 %s", trip.seat[0] ? trip.seat : "-");
    clipped_text(251, 388, line, 2, 201, BLACK);
    if (trip.note[0]) clipped_text(28, 441, trip.note, 1, 420, BLACK);
    rect(28, 487, 424, 3, BLUE);
    text(28, 514, "目的地天气", 1, BLUE);
    clipped_text(231, 514, trip.city, 1, 221, BLACK);
    text(28, 649, trip.arrival_date, 1, BLACK);
    page_trip_weather_t weather;
    if (page_trip_weather(&trip, &weather)) {
        weather_icon(75, 604, 34, weather.code);
        snprintf(line, sizeof(line), "%d/%d C", weather.high, weather.low);
        text(134, 565, line, 3, BLACK);
        if (weather.rain >= 0)
            snprintf(line, sizeof(line), "%s  降水 %d%%", weather_label(weather.code), weather.rain);
        else snprintf(line, sizeof(line), "%s", weather_label(weather.code));
        text(134, 626, line, 1, BLUE);
        text(337, 649, "OPEN-METEO", 1, BLACK);
    } else centered(240, 591, "暂无抵达日预报", 2, BLACK);
}

static void history_layout(const struct tm *now, int day)
{
    rect(29, 35, 5, 45, RED);
    text(47, 36, "历史上的今天", 2, BLACK);
    if (day < 0) {
        centered(240, 325, "等待时间同步", 2, BLUE);
        return;
    }
    char line[40];
    strftime(line, sizeof(line), "%m/%d", now);
    text(339, 47, line, 2, BLUE);
    rect(28, 109, 424, 2, BLACK);
    page_history_t item;
    if (!page_history_get(now, &item)) {
        centered(240, 310, "暂无今天的事件缓存", 2, BLACK);
        centered(240, 372, "设备联网后会自动更新", 1, BLUE);
        return;
    }
    if (item.year < 0) snprintf(line, sizeof(line), "公元前 %d 年", -item.year);
    else snprintf(line, sizeof(line), "%d 年", item.year);
    centered(240, 182, line, item.year < 0 ? 2 : 5, BLUE);
    snprintf(line, sizeof(line), "%d 年前", now->tm_year + 1900 - item.year);
    centered(240, 287, line, 1, RED);
    rect(64, 334, 352, 2, BLUE);
    const char *p = item.text;
    for (int i = 0, y = 365; i < 5 && *p; ++i, y += 48) {
        char row[80];
        const char *end = p;
        int width = 0;
        while (*end) {
            const char *next = end;
            width += (next_code(&next) < 128 ? 10 : 17) * 2;
            if (width > 415 || next - p >= sizeof(row)) break;
            end = next;
        }
        memcpy(row, p, end - p);
        row[end - p] = 0;
        text(32, y, row, 2, BLACK);
        p = end;
    }
    if (*p) text(32, 610, "...", 1, BLUE);
    text(30, 650, "来源  中文维基百科", 1, BLUE);
}

static bool ink(const draw_op_t *op, int dx, int dy)
{
    switch (op->shape) {
    case DASH: return dx % 14 < 8;
    case CIRCLE: {
        const int x = dx - op->scale, y = dy - op->scale;
        return x * x + y * y <= op->scale * op->scale;
    }
    case GLYPH:
        return (((const uint16_t *)op->bits)[dy / op->scale] >> (15 - dx / op->scale)) & 1;
    case BITMAP: {
        const int stride = op->scale ? TODO_TEXT_WIDTH : op->w;
        if (op->scale) {
            dx = dx * 100 / op->scale;
            dy = dy * 100 / op->scale;
        }
        return (((const uint8_t *)op->bits)[(dy * stride + dx) / 8] >> (7 - dx % 8)) & 1;
    }
    default:
        return true;
    }
}

static void paint_line(int fixed)
{
    memset(s_column, WHITE, sizeof(s_column));
    const bool landscape = s_width > s_height;
    const int origin = (s_width - 320) / 2;
    for (size_t i = 0; i < s_count; ++i) {
        const draw_op_t *op = &s_ops[i];
        if (landscape) {
            const int dy = fixed * 3 / 2 - op->y;
            if (dy < 0 || dy >= op->h) continue;
            const int begin = (op->x * 2 + 2) / 3;
            const int end = ((op->x + op->w) * 2 + 2) / 3;
            for (int x = begin; x < end && x + origin < s_width; ++x) {
                if (x + origin >= 0 && ink(op, x * 3 / 2 - op->x, dy)) {
                    s_column[x + origin] = s_mono && op->color != WHITE ? BLACK : op->color;
                }
            }
        } else {
            const int dx = fixed - op->x;
            if (dx < 0 || dx >= op->w) continue;
            for (int y = op->y; y < op->y + op->h && y < s_height; ++y) {
                if (y >= 0 && ink(op, dx, y - op->y))
                    s_column[y] = s_mono && op->color != WHITE ? BLACK : op->color;
            }
        }
    }
}

static uint8_t panel_pixel(int x, int y)
{
    int lx, ly;
    screen_rotation_source_xy(s_rotation, x, y, &lx, &ly);
    const bool landscape = s_width > s_height;
    const int fixed = landscape ? ly : lx;
    if (s_last_x != fixed) {
        paint_line(fixed);
        s_last_x = fixed;
    }
    return s_column[landscape ? lx : ly];
}

static esp_err_t read_frame(void *context, uint8_t *out, size_t capacity, size_t *received)
{
    (void)context;
    size_t remain = SCREEN_FRAME_BYTES - s_sent;
    *received = remain < capacity ? remain : capacity;
    for (size_t i = 0; i < *received; ++i, ++s_sent) {
        int x = SCREEN_WIDTH - 1 - (int)(s_sent / (SCREEN_HEIGHT / 2));
        int y = (int)(s_sent % (SCREEN_HEIGHT / 2)) * 2;
        uint8_t high = panel_pixel(x, y);
        out[i] = (high << 4) | panel_pixel(x, y + 1);
    }
    return ESP_OK;
}

esp_err_t dashboard_show(screen_mode_t mode, unsigned rotation)
{
    s_rotation = rotation;
    s_mono = (dashboard_mono_mask() & (1U << mode)) != 0;
    screen_rotation_size(rotation, &s_width, &s_height);
    s_count = 0;
    s_sent = 0;
    s_last_x = -1;
    int day = -1;
    if (mode == SCREEN_MODE_CALENDAR) calendar_layout();
    else if (mode == SCREEN_MODE_WEATHER) weather_layout();
    else if (mode == SCREEN_MODE_TODO) todo_layout();
    else if (mode == SCREEN_MODE_TRIP) trip_layout();
    else if (mode == SCREEN_MODE_QUOTE || mode == SCREEN_MODE_HISTORY) {
        struct tm local;
        day = quote_day(&local);
        if (mode == SCREEN_MODE_QUOTE) quote_layout(&local, day);
        else history_layout(&local, day);
    } else return ESP_ERR_INVALID_ARG;
    const int low = axp2101_low_battery_percent();
    if (low >= 0) {
        char message[28];
        snprintf(message, sizeof(message), "电量低 %d%%", low);
        centered(240, mode == SCREEN_MODE_WEATHER ? 659 : 684, message, 1, RED);
    }
    const esp_err_t err = screen_driver_show_stream(read_frame, NULL);
    if (err == ESP_OK && (mode == SCREEN_MODE_QUOTE || mode == SCREEN_MODE_HISTORY)) {
        __atomic_store_n(&s_quote_day, day, __ATOMIC_RELAXED);
    }
    return err;
}
