#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/inotify.h>
#include <pty.h>
#include <locale.h>
#include <errno.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <vterm.h>
#include <ctype.h>

#include "xdg-shell-client-protocol.h"
#include "../lib/jsmn.h"

#define DEFAULT_FONT_NAME    "DepartureMono"
#define DEFAULT_FONT_SIZE    12
#define DEFAULT_WIDTH        800
#define DEFAULT_HEIGHT       600

#define MAX_CUSTOM_BINDS     64
#define MAX_ACTIONS_PER_BIND 16

#define MIN_VTERM_COLS       80
#define MIN_VTERM_ROWS       24

#define SCROLLBACK_SIZE      1000
#define SCROLLBAR_WIDTH      6

typedef struct {
    xkb_keysym_t sym;
    int need_ctrl;
    int need_shift;
    int need_alt;
    char actions[MAX_ACTIONS_PER_BIND][256];
    int actions_count;
} KeyBind;

typedef struct {
    int row;
    int col;
    int len;
} SearchMatch;

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_buffer *buffer;
    struct wl_callback *frame_callback;

    struct wl_cursor_theme *cursor_theme;
    struct wl_cursor *cursor_text;
    struct wl_cursor *cursor_default;
    struct wl_surface *cursor_surface;
    int current_cursor_is_text;

    void *shm_data;
    size_t shm_size;
    cairo_surface_t *cairo_surface;
    cairo_t *cr;
    PangoLayout *pango_layout;
    PangoFontDescription *font_desc;

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    int pty_master;
    pid_t pid;
    VTerm *vterm;
    VTermScreen *vtscreen;

    int width, height;
    int cols, rows;
    int font_width, font_height;

    char *font_family;
    int font_size;
    char *font_name;
    uint32_t bg_color;
    uint32_t fg_color;
    int padding_x;
    int padding_y;
    char *shell;
    char *execute_cmd;
    double palette[16][3];
    KeyBind binds[MAX_CUSTOM_BINDS];
    int binds_count;

    int configured;
    int needs_render;

    double mouse_x, mouse_y;

    VTermScreenCell *scrollback[SCROLLBACK_SIZE];
    int scrollback_head;
    int scrollback_count;
    int scrollback_offset;

    int mouse_reporting;
    int left_button_pressed;
    int middle_button_pressed;
    double middle_drag_start_y;
    int middle_drag_start_offset;
    int is_altscreen;
    int dragging_scrollbar;

    uint32_t held_keycode;
    char held_bytes[32];
    int held_bytes_len;
    int repeat_delay_ms;
    int repeat_rate_hz;
    long long next_repeat_time_ms;
    int is_repeating;

    uint32_t active_bind_keycode;
    int cursor_shape;

    int inotify_fd;
    int inotify_wd;

    int search_active;
    char search_query[256];
    int search_query_len;
    SearchMatch *search_matches;
    int search_matches_count;
    int search_current_match;
} Term;

static Term app = {0};

static void render(void);
static void create_buffer(void);
static void cleanup_buffer(void);
static void resize_pty(void);
static void copy_selection_to_clipboard(void);
static void update_cursor_shape(uint32_t serial);
static const struct wl_callback_listener frame_listener;
static void update_search_matches(void);
static void clear_full_history(void);

static const double default_palette[16][3] = {
    { 0x5c/255.0, 0x4b/255.0, 0x4b/255.0 },
    { 0xe6/255.0, 0x7e/255.0, 0x80/255.0 },
    { 0x5c/255.0, 0x4b/255.0, 0x50/255.0 },
    { 0xe6/255.0, 0xa0/255.0, 0x80/255.0 },
    { 0xe6/255.0, 0x80/255.0, 0xa0/255.0 },
    { 0xe6/255.0, 0x80/255.0, 0xc0/255.0 },
    { 0xe6/255.0, 0x80/255.0, 0x80/255.0 },
    { 0xd3/255.0, 0xc6/255.0, 0xaa/255.0 },
    { 0x72/255.0, 0x5c/255.0, 0x5c/255.0 },
    { 0xf0/255.0, 0x94/255.0, 0x96/255.0 },
    { 0xf0/255.0, 0xa2/255.0, 0x94/255.0 },
    { 0xf0/255.0, 0xb5/255.0, 0x94/255.0 },
    { 0xf0/255.0, 0x94/255.0, 0xb5/255.0 },
    { 0xf0/255.0, 0x94/255.0, 0xd5/255.0 },
    { 0xf0/255.0, 0x94/255.0, 0x94/255.0 },
    { 0xf5/255.0, 0xe6/255.0, 0xe6/255.0 }
};

static inline int get_padding_right(void) {
    return app.padding_x + SCROLLBAR_WIDTH + 8;
}

static inline long long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static inline int ucs4_to_utf8(uint32_t ch, char *out) {
    if (ch == 0) {
        out[0] = ' ';
        return 1;
    }
    if (ch < 0x80) {
        out[0] = (char)ch;
        return 1;
    } else if (ch < 0x800) {
        out[0] = (char)(0xC0 | (ch >> 6));
        out[1] = (char)(0x80 | (ch & 0x3F));
        return 2;
    } else if (ch < 0x10000) {
        out[0] = (char)(0xE0 | (ch >> 12));
        out[1] = (char)(0x80 | ((ch >> 6) & 0x3F));
        out[2] = (char)(0x80 | (ch & 0x3F));
        return 3;
    } else if (ch <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (ch >> 18));
        out[1] = (char)(0x80 | ((ch >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((ch >> 6) & 0x3F));
        out[3] = (char)(0x80 | (ch & 0x3F));
        return 4;
    }
    out[0] = ' ';
    return 1;
}

static inline int get_cell_str(const VTermScreenCell *cell, char *out) {
    if (!cell->chars[0]) {
        out[0] = ' ';
        out[1] = '\0';
        return 1;
    }
    int len = 0;
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell->chars[i]; i++) {
        len += ucs4_to_utf8(cell->chars[i], out + len);
    }
    out[len] = '\0';
    return len;
}

static inline int get_cell_width(const VTermScreenCell *cell) {
    uint32_t ch = cell->chars[0];
    if (!ch || ch == ' ') return 1;
    if (ch >= 0xE000 && ch <= 0xF8FF) return 1;
    if ((ch >= 0xF0000 && ch <= 0xFFFFD) ||
        (ch >= 0x100000 && ch <= 0x10FFFD) ||
        (ch >= 0x2300 && ch <= 0x23FF) ||
        (ch >= 0x2B00 && ch <= 0x2BFF) ||
        (ch >= 0x2600 && ch <= 0x27BF) ||
        (ch >= 0x1F000 && ch <= 0x1FAFF)) {
        return 2;
    }
    return 1;
}

static int pango_x_to_col(int target_row, double mouse_x_rel) {
    if (target_row < 0 || target_row >= app.rows) return 0;
    double current_x = 0;
    for (int c = 0; c < app.cols; c++) {
        VTermScreenCell cell;
        int virtual_target = target_row - app.scrollback_offset;
        if (virtual_target < 0) {
            int sb_idx = (app.scrollback_head + virtual_target + SCROLLBACK_SIZE) % SCROLLBACK_SIZE;
            if (app.scrollback[sb_idx]) {
                cell = app.scrollback[sb_idx][c];
            } else {
                memset(&cell, 0, sizeof(cell));
                cell.chars[0] = ' ';
            }
        } else {
            vterm_screen_get_cell(app.vtscreen, (VTermPos){virtual_target, c}, &cell);
        }
        int cell_w = get_cell_width(&cell);
        double next_x = current_x + (app.font_width * cell_w);
        if (mouse_x_rel >= current_x && mouse_x_rel < next_x) return c;
        current_x = next_x;
        if (current_x > app.width) break;
    }
    return app.cols - 1;
}

static inline int jsmntyeq(const char *json, const jsmntok_t *tok, const char *s) {
    return (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
            strncmp(json + tok->start, s, tok->end - tok->start) == 0);
}

static inline uint32_t parse_hex_color(const char *str) {
    if (*str == '#') str++;
    return (uint32_t)strtol(str, NULL, 16);
}

static void strip_json_comments(char *buf) {
    int in_string = 0, in_line = 0, in_block = 0;
    for (int i = 0; buf[i] != '\0'; i++) {
        if (in_line) {
            if (buf[i] == '\n') in_line = 0;
            buf[i] = ' ';
            continue;
        }
        if (in_block) {
            if (buf[i] == '*' && buf[i+1] == '/') {
                buf[i] = buf[i+1] = ' ';
                in_block = 0;
                i++;
            } else {
                buf[i] = ' ';
            }
            continue;
        }
        if (!in_string) {
            if (buf[i] == '/' && buf[i+1] == '/') {
                buf[i] = buf[i+1] = ' ';
                in_line = 1;
                i++;
                continue;
            }
            if (buf[i] == '/' && buf[i+1] == '*') {
                buf[i] = buf[i+1] = ' ';
                in_block = 1;
                i++;
                continue;
            }
        }
        if (buf[i] == '"' && (i == 0 || buf[i-1] != '\\')) {
            in_string = !in_string;
        }
    }
}

static void clear_full_history(void) {
    for (int i = 0; i < SCROLLBACK_SIZE; i++) {
        if (app.scrollback[i]) {
            free(app.scrollback[i]);
            app.scrollback[i] = NULL;
        }
    }
    app.scrollback_head = 0;
    app.scrollback_count = 0;
    app.scrollback_offset = 0;
    if (app.vtscreen) vterm_screen_reset(app.vtscreen, 1);
    app.needs_render = 1;
}

static void load_config(void) {
    char path[1024];
    const char *home = getenv("HOME");
    snprintf(path, sizeof(path), "%s/.config/l/config.json", home ? home : "/root");

    free(app.font_family);
    free(app.font_name);
    free(app.shell);

    app.font_family = strdup(DEFAULT_FONT_NAME);
    app.font_size = DEFAULT_FONT_SIZE;
    app.bg_color = 0x282828;
    app.fg_color = 0xd3c6aa;
    app.padding_x = 6;
    app.padding_y = 6;
    app.shell = strdup("fish");
    app.binds_count = 0;
    app.cursor_shape = 0;
    memcpy(app.palette, default_palette, sizeof(default_palette));

    FILE *f = fopen(path, "r");
    if (!f) {
        system("mkdir -p ~/.config/l");
        FILE *df = fopen(path, "w");
        if (df) {
            fprintf(df, "{\n  \"font_family\": \"DepartureMono\",\n  \"font_size\": 12,\n  \"bg_color\": \"#282828\",\n  \"fg_color\": \"#d3c6aa\",\n  \"padding_x\": 6,\n  \"padding_y\": 6,\n  \"shell\": \"fish\",\n  \"binds\": [\n    { \"key\": \"c\", \"mods\": \"ctrl+shift\", \"action\": \"copy\" },\n    { \"key\": \"v\", \"mods\": \"ctrl+shift\", \"action\": \"paste\" },\n    { \"key\": \"equal\", \"mods\": \"ctrl\", \"action\": \"font_increase\" },\n    { \"key\": \"minus\", \"mods\": \"ctrl\", \"action\": \"font_decrease\" }\n  ]\n}\n");
            fclose(df);
        }
        f = fopen(path, "r");
        if (!f) {
            char full_font[128];
            snprintf(full_font, sizeof(full_font), "%s %d", app.font_family, app.font_size);
            app.font_name = strdup(full_font);
            return;
        }
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(f);
        char full_font[128];
        snprintf(full_font, sizeof(full_font), "%s %d", app.font_family, app.font_size);
        app.font_name = strdup(full_font);
        return;
    }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    strip_json_comments(buf);
    jsmn_parser p;
    jsmntok_t t[1024];
    jsmn_init(&p);
    int r = jsmn_parse(&p, buf, len, t, sizeof(t) / sizeof(t[0]));

    if (r > 0 && t[0].type == JSMN_OBJECT) {
        for (int i = 1; i < r; i++) {
            if (jsmntyeq(buf, &t[i], "font_family") && i + 1 < r) {
                free(app.font_family);
                app.font_family = strndup(buf + t[i+1].start, t[i+1].end - t[i+1].start);
                i++;
            } else if (jsmntyeq(buf, &t[i], "font_size") && i + 1 < r) {
                app.font_size = atoi(buf + t[i+1].start);
                i++;
            } else if (jsmntyeq(buf, &t[i], "bg_color") && i + 1 < r) {
                char *col = strndup(buf + t[i+1].start, t[i+1].end - t[i+1].start);
                app.bg_color = parse_hex_color(col);
                free(col);
                i++;
            } else if (jsmntyeq(buf, &t[i], "fg_color") && i + 1 < r) {
                char *col = strndup(buf + t[i+1].start, t[i+1].end - t[i+1].start);
                app.fg_color = parse_hex_color(col);
                free(col);
                i++;
            } else if (jsmntyeq(buf, &t[i], "padding_x") && i + 1 < r) {
                app.padding_x = atoi(buf + t[i+1].start);
                i++;
            } else if (jsmntyeq(buf, &t[i], "padding_y") && i + 1 < r) {
                app.padding_y = atoi(buf + t[i+1].start);
                i++;
            } else if (jsmntyeq(buf, &t[i], "shell") && i + 1 < r) {
                free(app.shell);
                app.shell = strndup(buf + t[i+1].start, t[i+1].end - t[i+1].start);
                i++;
            } else if (jsmntyeq(buf, &t[i], "binds") && i + 1 < r && t[i+1].type == JSMN_ARRAY) {
                int array_size = t[i+1].size;
                int idx = i + 2;
                for (int b = 0; b < array_size && app.binds_count < MAX_CUSTOM_BINDS; b++) {
                    if (idx >= r) break;
                    if (t[idx].type == JSMN_OBJECT) {
                        int obj_size = t[idx].size;
                        idx++;
                        char *k_str = NULL;
                        char *m_str = NULL;
                        KeyBind kb = {0};
                        for (int o = 0; o < obj_size; o++) {
                            if (idx >= r) break;
                            if (jsmntyeq(buf, &t[idx], "key") && idx + 1 < r) {
                                k_str = strndup(buf + t[idx+1].start, t[idx+1].end - t[idx+1].start);
                                idx += 2;
                            } else if (jsmntyeq(buf, &t[idx], "mods") && idx + 1 < r) {
                                m_str = strndup(buf + t[idx+1].start, t[idx+1].end - t[idx+1].start);
                                idx += 2;
                            } else if (jsmntyeq(buf, &t[idx], "action") && idx + 1 < r) {
                                if (t[idx+1].type == JSMN_STRING && kb.actions_count < MAX_ACTIONS_PER_BIND) {
                                    char *act = strndup(buf + t[idx+1].start, t[idx+1].end - t[idx+1].start);
                                    snprintf(kb.actions[kb.actions_count++], sizeof(kb.actions[0]), "%s", act);
                                    free(act);
                                }
                                idx += 2;
                            } else if (jsmntyeq(buf, &t[idx], "actions") && idx + 1 < r && t[idx+1].type == JSMN_ARRAY) {
                                int acts_count = t[idx+1].size;
                                idx += 2;
                                for (int a = 0; a < acts_count; a++) {
                                    if (idx < r && t[idx].type == JSMN_STRING && kb.actions_count < MAX_ACTIONS_PER_BIND) {
                                        char *act = strndup(buf + t[idx].start, t[idx].end - t[idx].start);
                                        snprintf(kb.actions[kb.actions_count++], sizeof(kb.actions[0]), "%s", act);
                                        free(act);
                                    }
                                    idx++;
                                }
                            } else {
                                idx += 2;
                            }
                        }
                        if (k_str && kb.actions_count > 0) {
                            kb.sym = xkb_keysym_from_name(k_str, XKB_KEYSYM_NO_FLAGS);
                            if (m_str) {
                                if (strstr(m_str, "ctrl")) kb.need_ctrl = 1;
                                if (strstr(m_str, "shift")) kb.need_shift = 1;
                                if (strstr(m_str, "alt")) kb.need_alt = 1;
                            }
                            app.binds[app.binds_count++] = kb;
                        }
                        free(k_str);
                        free(m_str);
                    } else {
                        idx++;
                    }
                }
                i = idx - 1;
            }
        }
    }
    char full_font[128];
    snprintf(full_font, sizeof(full_font), "%s %d", app.font_family, app.font_size);
    app.font_name = strdup(full_font);
    free(buf);
}

static void paste_from_clipboard(void) {
    FILE *pipe = popen("wl-paste -n", "r");
    if (!pipe) return;
    int in_alt = app.is_altscreen;

    if (in_alt) {
        int is_nvim_insert = (app.cursor_shape == 2);
        if (!is_nvim_insert) {
            write(app.pty_master, "i", 1);
            usleep(30000);
        }
    }

    write(app.pty_master, "\033[?2004h", 8); 
    
    char clip_buf[4096];
    size_t n;
    while ((n = fread(clip_buf, 1, sizeof(clip_buf), pipe)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (clip_buf[i] == '\n' && !in_alt) {
                write(app.pty_master, "\\\n", 2);
            } else {
                write(app.pty_master, &clip_buf[i], 1);
            }
        }
    }
    pclose(pipe);
    
    write(app.pty_master, "\033[?2004l", 8);

    app.scrollback_offset = 0;
    app.needs_render = 1;
}

static int create_shm_file(off_t size) {
    char template[] = "/tmp/l-term-shm-XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) return -1;
    unlink(template);
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void cleanup_buffer(void) {
    if (app.frame_callback) { wl_callback_destroy(app.frame_callback); app.frame_callback = NULL; }
    if (app.cr) { cairo_destroy(app.cr); app.cr = NULL; }
    if (app.pango_layout) { g_object_unref(app.pango_layout); app.pango_layout = NULL; }
    if (app.font_desc) { pango_font_description_free(app.font_desc); app.font_desc = NULL; }
    if (app.cairo_surface) { cairo_surface_destroy(app.cairo_surface); app.cairo_surface = NULL; }
    if (app.buffer) { wl_buffer_destroy(app.buffer); app.buffer = NULL; }
    if (app.shm_data && app.shm_size > 0) { munmap(app.shm_data, app.shm_size); app.shm_data = NULL; app.shm_size = 0; }
}

static void create_buffer(void) {
    if (app.width <= 0 || app.height <= 0) return;
    cleanup_buffer();
    int stride = app.width * 4;
    int size = stride * app.height;
    app.shm_size = size;
    int fd = create_shm_file(size);
    if (fd < 0) return;
    app.shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (app.shm_data == MAP_FAILED) { close(fd); app.shm_data = NULL; return; }
    memset(app.shm_data, 0, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(app.shm, fd, size);
    app.buffer = wl_shm_pool_create_buffer(pool, 0, app.width, app.height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    app.cairo_surface = cairo_image_surface_create_for_data(app.shm_data, CAIRO_FORMAT_ARGB32, app.width, app.height, stride);
    app.cr = cairo_create(app.cairo_surface);
    app.pango_layout = pango_cairo_create_layout(app.cr);
    app.font_desc = pango_font_description_from_string(app.font_name);
    pango_layout_set_font_description(app.pango_layout, app.font_desc);
    PangoContext *context = pango_layout_get_context(app.pango_layout);
    PangoFontMetrics *metrics = pango_context_get_metrics(context, app.font_desc, NULL);
    app.font_width = pango_font_metrics_get_approximate_char_width(metrics) / PANGO_SCALE;
    app.font_height = (pango_font_metrics_get_ascent(metrics) + pango_font_metrics_get_descent(metrics)) / PANGO_SCALE;
    pango_font_metrics_unref(metrics);
    if (app.font_width <= 0) app.font_width = 8;
    if (app.font_height <= 0) app.font_height = 16;
}

static inline void get_cell_colors(const VTermScreenCell *cell, double default_r, double default_g, double default_b,
                                   double bg_r, double bg_g, double bg_b,
                                   double *out_r, double *out_g, double *out_b,
                                   double *out_bg_r, double *out_bg_g, double *out_bg_b, int *has_bg) {
    double fg_r = default_r, fg_g = default_g, fg_b = default_b;
    double cell_bg_r = -1, cell_bg_g = -1, cell_bg_b = -1;
    int custom_bg = 0;

    if (cell->fg.type == VTERM_COLOR_RGB) {
        fg_r = cell->fg.rgb.red / 255.0;
        fg_g = cell->fg.rgb.green / 255.0;
        fg_b = cell->fg.rgb.blue / 255.0;
    } else if (cell->fg.type == VTERM_COLOR_INDEXED) {
        int idx = cell->fg.indexed.idx;
        if (idx >= 0 && idx < 16) {
            fg_r = app.palette[idx][0];
            fg_g = app.palette[idx][1];
            fg_b = app.palette[idx][2];
        }
    }

    if (cell->bg.type == VTERM_COLOR_RGB) {
        cell_bg_r = cell->bg.rgb.red / 255.0;
        cell_bg_g = cell->bg.rgb.green / 255.0;
        cell_bg_b = cell->bg.rgb.blue / 255.0;
        custom_bg = 1;
    } else if (cell->bg.type == VTERM_COLOR_INDEXED) {
        int idx = cell->bg.indexed.idx;
        if (idx >= 0 && idx < 16) {
            cell_bg_r = app.palette[idx][0];
            cell_bg_g = app.palette[idx][1];
            cell_bg_b = app.palette[idx][2];
            custom_bg = 1;
        }
    }

    if (cell->attrs.reverse) {
        double tmp_r = fg_r, tmp_g = fg_g, tmp_b = fg_b;
        fg_r = app.palette[0][0]; 
        fg_g = app.palette[0][1];
        fg_b = app.palette[0][2];
        cell_bg_r = tmp_r; 
        cell_bg_g = tmp_g;
        cell_bg_b = tmp_b;
        custom_bg = 1;
    }

    *out_r = fg_r; *out_g = fg_g; *out_b = fg_b;
    *out_bg_r = cell_bg_r; *out_bg_g = cell_bg_g; *out_bg_b = cell_bg_b;
    *has_bg = custom_bg;
}

static inline int is_cell_matched(int virtual_row, int c, int *is_current) {
    if (!app.search_active || app.search_query_len == 0) return 0;
    for (int i = 0; i < app.search_matches_count; i++) {
        const SearchMatch *m = &app.search_matches[i];
        if (m->row == virtual_row && c >= m->col && c < m->col + m->len) {
            if (is_current) *is_current = (i == app.search_current_match);
            return 1;
        }
    }
    return 0;
}

static char* my_strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

static void update_search_matches(void) {
    if (app.search_matches) { free(app.search_matches); app.search_matches = NULL; }
    app.search_matches_count = 0;
    app.search_current_match = -1;
    if (!app.search_active || app.search_query_len == 0 || app.is_altscreen) {
        app.needs_render = 1;
        return;
    }
    int has_uppercase = 0;
    for (int i = 0; i < app.search_query_len; i++) {
        if (isupper((unsigned char)app.search_query[i])) { has_uppercase = 1; break; }
    }
    int total_rows = app.scrollback_count + app.rows;
    SearchMatch *temp_matches = malloc(sizeof(SearchMatch) * total_rows);
    if (!temp_matches) return;
    int count = 0;
    for (int r = 0; r < total_rows; r++) {
        char line_chars[2048] = {0};
        int line_len = 0;
        for (int c = 0; c < app.cols; c++) {
            VTermScreenCell cell;
            int target_row = r - app.scrollback_count;
            if (target_row < 0) {
                int sb_idx = (app.scrollback_head + target_row + SCROLLBACK_SIZE) % SCROLLBACK_SIZE;
                if (app.scrollback[sb_idx]) cell = app.scrollback[sb_idx][c];
                else { memset(&cell, 0, sizeof(cell)); cell.chars[0] = ' '; }
            } else {
                vterm_screen_get_cell(app.vtscreen, (VTermPos){target_row, c}, &cell);
            }
            char utf8[16] = {0};
            int w = get_cell_str(&cell, utf8);
            if (line_len + w < (int)sizeof(line_chars) - 1) {
                memcpy(line_chars + line_len, utf8, w);
                line_len += w;
            }
        }
        line_chars[line_len] = '\0';
        if (has_uppercase ? strstr(line_chars, app.search_query) : my_strcasestr(line_chars, app.search_query)) {
            temp_matches[count++] = (SearchMatch){.row = r, .col = 0, .len = app.cols};
        }
    }
    if (count > 0) {
        app.search_matches = malloc(sizeof(SearchMatch) * count);
        if (app.search_matches) {
            memcpy(app.search_matches, temp_matches, sizeof(SearchMatch) * count);
            app.search_matches_count = count;
            app.search_current_match = count - 1;
            int screen_row = app.search_matches[app.search_current_match].row - app.scrollback_count;
            if (screen_row < 0) {
                app.scrollback_offset = -screen_row;
                if (app.scrollback_offset > app.scrollback_count) app.scrollback_offset = app.scrollback_count;
            } else if (screen_row >= app.rows) {
                int shift = screen_row - (app.rows - 1);
                if (shift < 1) shift = 1;
                app.scrollback_offset += shift;
                if (app.scrollback_offset > app.scrollback_count) app.scrollback_offset = app.scrollback_count;
            } else {
                app.scrollback_offset = 0;
            }
        }
    }
    free(temp_matches);
    app.needs_render = 1;
}

static void copy_selection_to_clipboard(void) {
}

static void update_cursor_shape(uint32_t serial) {
    if (!app.pointer) return;
    struct wl_cursor *cursor = app.cursor_default;
    int is_text = 0;
    if (!app.is_altscreen) {
        int pr = get_padding_right();
        double text_right_bound = (double)app.width - pr;
        if (app.mouse_x >= app.padding_x && app.mouse_x < text_right_bound &&
            app.mouse_y >= app.padding_y && app.mouse_y < app.height - app.padding_y) {
            is_text = 1;
            cursor = app.cursor_text ? app.cursor_text : app.cursor_default;
        }
    }
    app.current_cursor_is_text = is_text;
    if (cursor && cursor->image_count > 0) {
        struct wl_cursor_image *image = cursor->images[0];
        if (!app.cursor_surface) app.cursor_surface = wl_compositor_create_surface(app.compositor);
        wl_surface_attach(app.cursor_surface, wl_cursor_image_get_buffer(image), 0, 0);
        wl_surface_damage(app.cursor_surface, 0, 0, image->width, image->height);
        wl_surface_commit(app.cursor_surface);
        wl_pointer_set_cursor(app.pointer, serial, app.cursor_surface, image->hotspot_x, image->hotspot_y);
    }
}

static void render(void) {
    if (!app.configured || !app.cr || !app.surface || !app.buffer || !app.vtscreen || !app.pango_layout) return;

    double bg_r = ((app.bg_color >> 16) & 0xFF) / 255.0;
    double bg_g = ((app.bg_color >> 8) & 0xFF) / 255.0;
    double bg_b = (app.bg_color & 0xFF) / 255.0;

    cairo_save(app.cr);
    cairo_set_operator(app.cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(app.cr, bg_r, bg_g, bg_b);
    cairo_paint(app.cr);
    cairo_restore(app.cr);

    cairo_save(app.cr);
    cairo_translate(app.cr, (double)app.padding_x, (double)app.padding_y);

    double default_fg_r = ((app.fg_color >> 16) & 0xFF) / 255.0;
    double default_fg_g = ((app.fg_color >> 8) & 0xFF) / 255.0;
    double default_fg_b = (app.fg_color & 0xFF) / 255.0;
    double panel_height = (double)(app.font_height * 2);

    for (int row = 0; row < app.rows; row++) {
        int target_row = row - app.scrollback_offset;
        int virtual_row = app.scrollback_count + target_row;
        int sb_idx = (target_row < 0) ? (app.scrollback_head + target_row + SCROLLBACK_SIZE) % SCROLLBACK_SIZE : 0;
        const VTermScreenCell *sb_row = (target_row < 0) ? app.scrollback[sb_idx] : NULL;

        for (int col = 0; col < app.cols; col++) {
            VTermScreenCell cell;
            if (target_row < 0) {
                if (sb_row) cell = sb_row[col];
                else { memset(&cell, 0, sizeof(cell)); cell.chars[0] = ' '; }
            } else {
                vterm_screen_get_cell(app.vtscreen, (VTermPos){target_row, col}, &cell);
            }

            double cr, cg, cb, cbg_r, cbg_g, cbg_b;
            int has_bg = 0;
            get_cell_colors(&cell, default_fg_r, default_fg_g, default_fg_b, bg_r, bg_g, bg_b, 
                            &cr, &cg, &cb, &cbg_r, &cbg_g, &cbg_b, &has_bg);

            int is_underline = (cell.attrs.underline != 0);
            int is_current_match = 0;
            int matched = is_cell_matched(virtual_row, col, &is_current_match);
            int has_char = (cell.chars[0] && cell.chars[0] != ' ');

            if (!has_char && !has_bg && !is_underline && !matched) continue;

            int cell_w = get_cell_width(&cell);
            double x = (double)(col * app.font_width);
            double y = (double)(row * app.font_height);
            double fw = (double)(app.font_width * cell_w);
            double fh = (double)app.font_height;

            if (matched) {
                if (is_current_match) cairo_set_source_rgb(app.cr, 0.9, 0.5, 0.2);
                else cairo_set_source_rgb(app.cr, 0.8, 0.8, 0.2);
                cairo_rectangle(app.cr, x, y, fw, fh);
                cairo_fill(app.cr);
            } else if (has_bg) {
                cairo_set_source_rgb(app.cr, cbg_r, cbg_g, cbg_b);
                cairo_rectangle(app.cr, x, y, fw, fh);
                cairo_fill(app.cr);
            }

            if (has_char) {
                char utf8[16] = {0};
                int len = get_cell_str(&cell, utf8);
                if (matched) {
                    cairo_set_source_rgb(app.cr, bg_r, bg_g, bg_b);
                } else {
                    double e_bg_r = has_bg ? cbg_r : bg_r;
                    double e_bg_g = has_bg ? cbg_g : bg_g;
                    double e_bg_b = has_bg ? cbg_b : bg_b;
                    if (((cr - e_bg_r)*(cr - e_bg_r) + (cg - e_bg_g)*(cg - e_bg_g) + (cb - e_bg_b)*(cb - e_bg_b)) < 0.05) {
                        cairo_set_source_rgb(app.cr, default_fg_r, default_fg_g, default_fg_b);
                    } else {
                        cairo_set_source_rgb(app.cr, cr, cg, cb);
                    }
                }
                cairo_move_to(app.cr, x, y);
                pango_layout_set_text(app.pango_layout, utf8, len);
                pango_cairo_show_layout(app.cr, app.pango_layout);
            }

            if (is_underline) {
                cairo_set_source_rgb(app.cr, cr, cg, cb);
                cairo_set_line_width(app.cr, 1.2);
                cairo_move_to(app.cr, x, y + fh - 1.0);
                cairo_line_to(app.cr, x + fw, y + fh - 1.0);
                cairo_stroke(app.cr);
            }

            if (cell_w == 2) col++;
        }
    }

    if (app.scrollback_offset == 0 && !app.search_active) {
        VTermPos cursor_pos;
        vterm_state_get_cursorpos(vterm_obtain_state(app.vterm), &cursor_pos);
        VTermScreenCell cursor_cell;
        vterm_screen_get_cell(app.vtscreen, cursor_pos, &cursor_cell);

        double cursor_x = (double)(cursor_pos.col * app.font_width);
        double cursor_y = (double)(cursor_pos.row * app.font_height);
        double fw = (double)app.font_width;
        double fh = (double)app.font_height;

        cairo_set_source_rgb(app.cr, app.palette[1][0], app.palette[1][1], app.palette[1][2]);
        if (app.cursor_shape == 2) {
            cairo_rectangle(app.cr, cursor_x, cursor_y, 2.0, fh);
            cairo_fill(app.cr);
        } else if (app.cursor_shape == 1) {
            cairo_rectangle(app.cr, cursor_x, cursor_y + fh - 3.0, fw, 3.0);
            cairo_fill(app.cr);
        } else {
            cairo_rectangle(app.cr, cursor_x, cursor_y, fw, fh);
            cairo_fill(app.cr);
            if (cursor_cell.chars[0] && cursor_cell.chars[0] != ' ') {
                char ch_buf[16] = {0};
                int ch_len = get_cell_str(&cursor_cell, ch_buf);
                cairo_set_source_rgb(app.cr, bg_r, bg_g, bg_b);
                cairo_move_to(app.cr, cursor_x, cursor_y);
                pango_layout_set_text(app.pango_layout, ch_buf, ch_len);
                pango_cairo_show_layout(app.cr, app.pango_layout);
            }
        }
    }

    cairo_restore(app.cr);

    if (app.search_active && !app.is_altscreen) {
        double panel_y = (double)app.height - panel_height - (double)app.padding_y;
        cairo_save(app.cr);
        cairo_set_source_rgb(app.cr, bg_r, bg_g, bg_b);
        cairo_rectangle(app.cr, (double)app.padding_x, panel_y, (double)app.width - (2.0 * app.padding_x), panel_height);
        cairo_fill(app.cr);
        cairo_set_source_rgba(app.cr, 0.15, 0.15, 0.15, 0.95);
        cairo_rectangle(app.cr, (double)app.padding_x, panel_y, (double)app.width - (2.0 * app.padding_x), panel_height);
        cairo_fill(app.cr);

        char panel_text[512];
        if (app.search_matches_count > 0) {
            snprintf(panel_text, sizeof(panel_text), "Search: %s [%d/%d]", app.search_query, app.search_current_match + 1, app.search_matches_count);
        } else {
            snprintf(panel_text, sizeof(panel_text), "Search: %s [No matches]", app.search_query);
        }
        cairo_set_source_rgb(app.cr, default_fg_r, default_fg_g, default_fg_b);
        cairo_move_to(app.cr, (double)app.padding_x + 10.0, panel_y + 4.0);
        pango_layout_set_text(app.pango_layout, panel_text, strlen(panel_text));
        pango_cairo_show_layout(app.cr, app.pango_layout);

        char query_part[512];
        snprintf(query_part, sizeof(query_part), "Search: %s", app.search_query);
        int query_width_px = 0;
        PangoLayout *layout_q = pango_cairo_create_layout(app.cr);
        pango_layout_set_font_description(layout_q, app.font_desc);
        pango_layout_set_text(layout_q, query_part, strlen(query_part));
        pango_layout_get_pixel_size(layout_q, &query_width_px, NULL);
        g_object_unref(layout_q);

        cairo_set_source_rgb(app.cr, app.palette[1][0], app.palette[1][1], app.palette[1][2]);
        cairo_rectangle(app.cr, (double)app.padding_x + 10.0 + (double)query_width_px, panel_y + 4.0, 2.0, (double)app.font_height);
        cairo_fill(app.cr);
        cairo_restore(app.cr);
    }

    if (!app.is_altscreen && app.scrollback_count > 0) {
        double content_height = (double)(app.rows * app.font_height);
        double total_lines = (double)(app.rows + app.scrollback_count);
        double thumb_height = (double)app.rows / total_lines * content_height;
        if (thumb_height < 20.0) thumb_height = 20.0;
        double scroll_ratio = (app.scrollback_count > 0) ? ((double)app.scrollback_offset / (double)app.scrollback_count) : 0.0;
        double thumb_y = (content_height - thumb_height) * (1.0 - scroll_ratio);
        double sb_w = SCROLLBAR_WIDTH;
        double sb_x = (double)app.width - sb_w - 6;
        int is_hovered = app.dragging_scrollbar || (app.mouse_x >= sb_x - 4 && app.mouse_x <= (double)app.width &&
                          app.mouse_y >= app.padding_y && app.mouse_y <= app.padding_y + content_height);
        double active_sb_width = is_hovered ? (sb_w + 3) : sb_w;
        double render_sb_x = (double)app.width - active_sb_width - 4;

        cairo_set_source_rgba(app.cr, 0.5, 0.5, 0.5, is_hovered ? 0.35 : 0.15);
        cairo_rectangle(app.cr, render_sb_x, (double)app.padding_y, active_sb_width, content_height);
        cairo_fill(app.cr);
        cairo_set_source_rgba(app.cr, 0.9, 0.9, 0.9, is_hovered ? 0.9 : 0.5);
        cairo_rectangle(app.cr, render_sb_x, (double)app.padding_y + thumb_y, active_sb_width, thumb_height);
        cairo_fill(app.cr);
    }

    wl_surface_attach(app.surface, app.buffer, 0, 0);
    wl_surface_damage(app.surface, 0, 0, app.width, app.height);
    if (!app.frame_callback) {
        app.frame_callback = wl_surface_frame(app.surface);
        wl_callback_add_listener(app.frame_callback, &frame_listener, NULL);
    }
    wl_surface_commit(app.surface);
    app.needs_render = 0;
}

static void frame_handle_done(void *data, struct wl_callback *callback, uint32_t time) {
    wl_callback_destroy(callback);
    app.frame_callback = NULL;
    if (app.needs_render) {
        render();
    } else {
        app.frame_callback = wl_surface_frame(app.surface);
        wl_callback_add_listener(app.frame_callback, &frame_listener, NULL);
        wl_surface_commit(app.surface);
    }
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_handle_done,
};

static int term_damage(VTermRect rect, void *user) { app.needs_render = 1; return 1; }
static int term_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user) { app.needs_render = 1; return 1; }
static int term_settermprop(VTermProp prop, VTermValue *val, void *user) {
    if (prop == VTERM_PROP_CURSORSHAPE) {
        app.cursor_shape = (val->number >= 3) ? 2 : (val->number == 2 ? 1 : 0);
        app.needs_render = 1;
        return 1;
    }
    return 0;
}

static int term_sb_pushline(int cols, const VTermScreenCell *cells, void *user) {
    if (app.scrollback[app.scrollback_head] == NULL) {
        app.scrollback[app.scrollback_head] = malloc(sizeof(VTermScreenCell) * cols);
    } else {
        app.scrollback[app.scrollback_head] = realloc(app.scrollback[app.scrollback_head], sizeof(VTermScreenCell) * cols);
    }
    if (app.scrollback[app.scrollback_head]) {
        memcpy(app.scrollback[app.scrollback_head], cells, sizeof(VTermScreenCell) * cols);
    }
    app.scrollback_head = (app.scrollback_head + 1) % SCROLLBACK_SIZE;
    if (app.scrollback_count < SCROLLBACK_SIZE) app.scrollback_count++;
    return 1;
}

static int term_sb_popline(int cols, VTermScreenCell *cells, void *user) {
    if (app.scrollback_count <= 0) return 0;
    app.scrollback_head = (app.scrollback_head - 1 + SCROLLBACK_SIZE) % SCROLLBACK_SIZE;
    app.scrollback_count--;
    if (app.scrollback[app.scrollback_head]) {
        memcpy(cells, app.scrollback[app.scrollback_head], sizeof(VTermScreenCell) * cols);
        return 1;
    }
    return 0;
}

static void term_output(const char *s, size_t len, void *user) {
    if (app.pty_master > 0) write(app.pty_master, s, len);
}

static VTermScreenCallbacks vt_callbacks = {
    .damage      = term_damage,
    .movecursor  = term_movecursor,
    .settermprop = term_settermprop,
    .sb_pushline = term_sb_pushline,
    .sb_popline  = term_sb_popline,
};

static void resize_pty(void) {
    if (app.pty_master <= 0) return;
    struct winsize ws = {0};
    ws.ws_row = app.rows;
    ws.ws_col = app.cols;
    ws.ws_xpixel = app.width - app.padding_x - get_padding_right();
    ws.ws_ypixel = app.height - (app.padding_y * 2);
    ioctl(app.pty_master, TIOCSWINSZ, &ws);
}

static void update_terminal_size_for_search(void) {
    int calc_rows = (app.height - (app.padding_y * 2)) / (app.font_height > 0 ? app.font_height : 16);
    int new_rows = calc_rows < MIN_VTERM_ROWS ? MIN_VTERM_ROWS : calc_rows;
    if (app.search_active && !app.is_altscreen) {
        new_rows -= 2;
        if (new_rows < 1) new_rows = 1;
    }
    if (new_rows != app.rows) {
        app.rows = new_rows;
        if (app.vterm) vterm_set_size(app.vterm, app.rows, app.cols);
        resize_pty();
    }
}

static void send_mouse_report(int button, int release, int col, int row) {
    if (!app.mouse_reporting) return;
    char seq[32];
    int cb = release ? 3 : button;
    int sgr_btn = (button == 64) ? 64 : (button == 65) ? 65 : cb;
    snprintf(seq, sizeof(seq), "\033[<%d;%d;%d%c", sgr_btn, col + 1, row + 1, release ? 'm' : 'M');
    write(app.pty_master, seq, strlen(seq));
}

static void change_font_size(int delta) {
    app.font_size += delta;
    if (app.font_size < 6) app.font_size = 6;
    if (app.font_size > 72) app.font_size = 72;
    char new_font[128];
    snprintf(new_font, sizeof(new_font), "%s %d", app.font_family, app.font_size);
    free(app.font_name);
    app.font_name = strdup(new_font);
    create_buffer();
    int pr = get_padding_right();
    int calc_cols = (app.width - app.padding_x - pr) / (app.font_width > 0 ? app.font_width : 8);
    int calc_rows = (app.height - (app.padding_y * 2)) / (app.font_height > 0 ? app.font_height : 16);
    app.cols = calc_cols < MIN_VTERM_COLS ? MIN_VTERM_COLS : calc_cols;
    app.rows = calc_rows < MIN_VTERM_ROWS ? MIN_VTERM_ROWS : calc_rows;
    if (app.search_active && !app.is_altscreen) {
        app.rows -= 2;
        if (app.rows < 1) app.rows = 1;
    }
    if (app.vterm) vterm_set_size(app.vterm, app.rows, app.cols);
    resize_pty();
    render();
}

static void xdg_surface_handle_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    xdg_surface_ack_configure(xdg_surface, serial);
    app.configured = 1;
    render();
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_handle_configure,
};

static void xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states) {
    if (width > 0 && height > 0) {
        app.width = width;
        app.height = height;
    } else if (app.width == 0 || app.height == 0) {
        app.width = DEFAULT_WIDTH;
        app.height = DEFAULT_HEIGHT;
    }
    create_buffer();
    int pr = get_padding_right();
    int calc_cols = (app.width - app.padding_x - pr) / (app.font_width > 0 ? app.font_width : 8);
    int calc_rows = (app.height - (app.padding_y * 2)) / (app.font_height > 0 ? app.font_height : 16);
    int new_cols = calc_cols < MIN_VTERM_COLS ? MIN_VTERM_COLS : calc_cols;
    int new_rows = calc_rows < MIN_VTERM_ROWS ? MIN_VTERM_ROWS : calc_rows;
    if (app.search_active && !app.is_altscreen) {
        new_rows -= 2;
        if (new_rows < 1) new_rows = 1;
    }
    if (new_cols != app.cols || new_rows != app.rows) {
        app.cols = new_cols;
        app.rows = new_rows;
        if (app.vterm) vterm_set_size(app.vterm, app.rows, app.cols);
        resize_pty();
    }
    app.needs_render = 1;
}

static void xdg_toplevel_handle_close(void *data, struct xdg_toplevel *toplevel) { exit(0); }

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_handle_configure,
    .close     = xdg_toplevel_handle_close,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial) { xdg_wm_base_pong(shell, serial); }

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void pointer_handle_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    app.mouse_x = wl_fixed_to_double(sx);
    app.mouse_y = wl_fixed_to_double(sy);
    update_cursor_shape(serial);
}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {
    app.needs_render = 1;
}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    app.mouse_x = wl_fixed_to_double(sx);
    app.mouse_y = wl_fixed_to_double(sy);
    app.needs_render = 1;

    int pr = get_padding_right();
    int is_text = (!app.is_altscreen && app.mouse_x >= app.padding_x && app.mouse_x < (double)app.width - pr &&
                   app.mouse_y >= app.padding_y && app.mouse_y < app.height - app.padding_y);
    if (is_text != app.current_cursor_is_text) update_cursor_shape(time);

    if (app.dragging_scrollbar && !app.is_altscreen && app.scrollback_count > 0) {
        double content_height = (double)(app.rows * app.font_height);
        double rel_y = app.mouse_y - app.padding_y;
        if (rel_y < 0) rel_y = 0;
        if (rel_y > content_height) rel_y = content_height;
        int new_offset = (int)((1.0 - (rel_y / content_height)) * (double)app.scrollback_count);
        if (new_offset < 0) new_offset = 0;
        if (new_offset > app.scrollback_count) new_offset = app.scrollback_count;
        if (new_offset != app.scrollback_offset) {
            app.scrollback_offset = new_offset;
        }
        return;
    }

    if (app.middle_button_pressed && !app.is_altscreen && app.scrollback_count > 0) {
        int new_offset = app.middle_drag_start_offset + (int)((app.mouse_y - app.middle_drag_start_y) / (double)app.font_height);
        if (new_offset < 0) new_offset = 0;
        if (new_offset > app.scrollback_count) new_offset = app.scrollback_count;
        if (new_offset != app.scrollback_offset) {
            app.scrollback_offset = new_offset;
        }
        app.needs_render = 1;
        return;
    }

    int row = (int)((app.mouse_y - app.padding_y) / app.font_height);
    if (row < 0) row = 0;
    if (row >= app.rows) row = app.rows - 1;
    int col = pango_x_to_col(row, app.mouse_x - app.padding_x);
    if (col < 0) col = 0;
    if (col >= app.cols) col = app.cols - 1;

    if (app.mouse_reporting) {
        if (app.left_button_pressed) send_mouse_report(32, 0, col, row);
    }
}

static void pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    if (button == 0x110) {
        int row = (int)((app.mouse_y - app.padding_y) / app.font_height);
        if (row < 0) row = 0;
        if (row >= app.rows) row = app.rows - 1;
        int col = pango_x_to_col(row, app.mouse_x - app.padding_x);
        if (col < 0) col = 0;
        if (col >= app.cols) col = app.cols - 1;

        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            app.left_button_pressed = 1;
            if (!app.is_altscreen && app.scrollback_count > 0 && app.mouse_x >= (double)app.width - SCROLLBAR_WIDTH - 12) {
                app.dragging_scrollbar = 1;
                double content_height = (double)(app.rows * app.font_height);
                double rel_y = app.mouse_y - app.padding_y;
                if (rel_y < 0) rel_y = 0;
                if (rel_y > content_height) rel_y = content_height;
                int new_offset = (int)((1.0 - (rel_y / content_height)) * (double)app.scrollback_count);
                if (new_offset != app.scrollback_offset) {
                    app.scrollback_offset = new_offset;
                }
                app.needs_render = 1;
                return;
            }
            if (app.mouse_reporting) {
                send_mouse_report(0, 0, col, row);
            }
        } else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
            app.left_button_pressed = 0;
            app.dragging_scrollbar = 0;
            if (app.mouse_reporting) send_mouse_report(0, 1, col, row);
        }
        app.needs_render = 1;
    } else if (button == 0x111) {
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            app.middle_button_pressed = 1;
            app.middle_drag_start_y = app.mouse_y;
            app.middle_drag_start_offset = app.scrollback_offset;
        } else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
            app.middle_button_pressed = 0;
        }
        app.needs_render = 1;
    }
}

static void pointer_handle_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
    double delta = wl_fixed_to_double(value);
    if (delta == 0) return;
    if (app.is_altscreen && app.mouse_reporting) {
        int row = (int)((app.mouse_y - app.padding_y) / app.font_height);
        int col = pango_x_to_col(row, app.mouse_x - app.padding_x);
        send_mouse_report((delta > 0) ? 65 : 64, 0, col, row);
        return;
    }
    int new_offset = app.scrollback_offset + (delta > 0 ? -3 : 3);
    if (new_offset < 0) new_offset = 0;
    if (new_offset > app.scrollback_count) new_offset = app.scrollback_count;
    if (new_offset != app.scrollback_offset) {
        app.scrollback_offset = new_offset;
    }
    app.needs_render = 1;
}

static void pointer_handle_frame(void *data, struct wl_pointer *pointer) {}
static void pointer_handle_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source) {}
static void pointer_handle_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {}
static void pointer_handle_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {
    if (discrete == 0) return;
    if (app.is_altscreen && app.mouse_reporting) {
        int row = (int)((app.mouse_y - app.padding_y) / app.font_height);
        int col = pango_x_to_col(row, app.mouse_x - app.padding_x);
        int btn = (discrete > 0) ? 65 : 64;
        for (int i = 0; i < abs(discrete); i++) send_mouse_report(btn, 0, col, row);
        return;
    }
    int new_offset = app.scrollback_offset + (discrete * -3);
    if (new_offset < 0) new_offset = 0;
    if (new_offset > app.scrollback_count) new_offset = app.scrollback_count;
    if (new_offset != app.scrollback_offset) {
        app.scrollback_offset = new_offset;
    }
    app.needs_render = 1;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter          = pointer_handle_enter,
    .leave          = pointer_handle_leave,
    .motion         = pointer_handle_motion,
    .button         = pointer_handle_button,
    .axis           = pointer_handle_axis,
    .frame          = pointer_handle_frame,
    .axis_source    = pointer_handle_axis_source,
    .axis_stop      = pointer_handle_axis_stop,
    .axis_discrete  = pointer_handle_axis_discrete,
};

static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int fd, uint32_t size) {
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_str == MAP_FAILED) { close(fd); return; }
    app.xkb_keymap = xkb_keymap_new_from_string(app.xkb_ctx, map_str, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    munmap(map_str, size);
    close(fd);
    app.xkb_state = xkb_state_new(app.xkb_keymap);
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    uint32_t keycode = key + 8;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (app.held_keycode == keycode) { app.held_keycode = 0; app.held_bytes_len = 0; }
        if (app.active_bind_keycode == keycode) { app.active_bind_keycode = 0; }
        return;
    }

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        uint32_t mods = xkb_state_serialize_mods(app.xkb_state, XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED | XKB_STATE_MODS_EFFECTIVE);
        xkb_mod_index_t ctrl_idx = xkb_keymap_mod_get_index(app.xkb_keymap, XKB_MOD_NAME_CTRL);
        xkb_mod_index_t alt_idx = xkb_keymap_mod_get_index(app.xkb_keymap, XKB_MOD_NAME_ALT);
        xkb_mod_index_t shift_idx = xkb_keymap_mod_get_index(app.xkb_keymap, XKB_MOD_NAME_SHIFT);

        int ctrl = (ctrl_idx != XKB_MOD_INVALID && (mods & (1 << ctrl_idx))) ? 1 : 0;
        int alt = (alt_idx != XKB_MOD_INVALID && (mods & (1 << alt_idx))) ? 1 : 0;
        int shift = (shift_idx != XKB_MOD_INVALID && (mods & (1 << shift_idx))) ? 1 : 0;

        xkb_keysym_t sym = app.xkb_state ? xkb_state_key_get_one_sym(app.xkb_state, keycode) : XKB_KEY_NoSymbol;
        if (sym == XKB_KEY_Shift_L || sym == XKB_KEY_Shift_R || sym == XKB_KEY_Control_L || sym == XKB_KEY_Control_R ||
            sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R || sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R) return;

        if (ctrl && shift && (sym == XKB_KEY_c || sym == XKB_KEY_C)) { copy_selection_to_clipboard(); return; }
        if (ctrl && shift && (sym == XKB_KEY_v || sym == XKB_KEY_V)) { paste_from_clipboard(); return; }

        xkb_keysym_t check_sym = xkb_keysym_to_lower(sym);
        for (int i = 0; i < app.binds_count; i++) {
            if (xkb_keysym_to_lower(app.binds[i].sym) == check_sym &&
                app.binds[i].need_ctrl == ctrl && app.binds[i].need_shift == shift && app.binds[i].need_alt == alt) {
                if (app.active_bind_keycode == keycode) return;
                app.active_bind_keycode = keycode;
                app.held_keycode = 0;
                app.held_bytes_len = 0;

                for (int a = 0; a < app.binds[i].actions_count; a++) {
                    const char *action = app.binds[i].actions[a];
                    if (strcmp(action, "copy") == 0) copy_selection_to_clipboard();
                    else if (strcmp(action, "paste") == 0) paste_from_clipboard();
                    else if (strcmp(action, "font_increase") == 0) change_font_size(1);
                    else if (strcmp(action, "font_decrease") == 0) change_font_size(-1);
                    else if (strcasecmp(action, "Return") == 0 || strcasecmp(action, "Enter") == 0) write(app.pty_master, "\r", 1);
                    else {
                        for (int j = 0; action[j] != '\0'; j++) {
                            if (action[j] == '\\' && action[j+1] != '\0') {
                                char next = action[j+1];
                                if (next == 'n' || next == 'r') { write(app.pty_master, "\r", 1); j++; }
                                else if (next == 't') { write(app.pty_master, "\t", 1); j++; }
                                else if (next == 'b') { write(app.pty_master, "\x7f", 1); j++; }
                                else if (next == 'e') { write(app.pty_master, "\x1b", 1); j++; }
                                else write(app.pty_master, &action[j], 1);
                            } else if (action[j] == '\n') {
                                write(app.pty_master, "\r", 1);
                            } else {
                                write(app.pty_master, &action[j], 1);
                            }
                        }
                    }
                }
                return;
            }
        }

        if (app.search_active && !app.is_altscreen) {
            if (sym == XKB_KEY_Escape) {
                app.search_active = 0;
                app.search_query[0] = '\0';
                app.search_query_len = 0;
                free(app.search_matches);
                app.search_matches = NULL;
                app.search_matches_count = 0;
                update_terminal_size_for_search();
                app.needs_render = 1;
                return;
            } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
                if (app.search_matches_count > 0) {
                    if (shift) {
                        app.search_current_match--;
                        if (app.search_current_match < 0) app.search_current_match = app.search_matches_count - 1;
                    } else {
                        app.search_current_match++;
                        if (app.search_current_match >= app.search_matches_count) app.search_current_match = 0;
                    }
                    int screen_row = app.search_matches[app.search_current_match].row - app.scrollback_count;
                    if (screen_row < 0) {
                        app.scrollback_offset = -screen_row;
                        if (app.scrollback_offset > app.scrollback_count) app.scrollback_offset = app.scrollback_count;
                    } else if (screen_row >= app.rows) {
                        int shift_val = screen_row - (app.rows - 1);
                        if (shift_val < 1) shift_val = 1;
                        app.scrollback_offset += shift_val;
                        if (app.scrollback_offset > app.scrollback_count) app.scrollback_offset = app.scrollback_count;
                    } else {
                        app.scrollback_offset = 0;
                    }
                    app.needs_render = 1;
                }
                return;
            } else if ((ctrl && (sym == XKB_KEY_w || sym == XKB_KEY_W || keycode == 22 || sym == 65288)) || keycode == 22 || sym == 65288 || sym == XKB_KEY_BackSpace) {
                if (app.search_query_len > 0) {
                    if (ctrl && (sym == XKB_KEY_w || sym == XKB_KEY_W || sym == 65288 || keycode == 22)) {
                        while (app.search_query_len > 0 && app.search_query[app.search_query_len - 1] == ' ') app.search_query[--app.search_query_len] = '\0';
                        while (app.search_query_len > 0 && app.search_query[app.search_query_len - 1] != ' ') app.search_query[--app.search_query_len] = '\0';
                    } else {
                        app.search_query[--app.search_query_len] = '\0';
                    }
                    update_search_matches();
                }
                app.held_keycode = keycode;
                app.held_bytes_len = 0;
                app.next_repeat_time_ms = get_time_ms() + (app.repeat_delay_ms > 0 ? app.repeat_delay_ms : 250);
                app.is_repeating = 0;
                return;
            } else {
                char buf[32] = {0};
                int len = app.xkb_state ? xkb_state_key_get_utf8(app.xkb_state, keycode, buf, sizeof(buf)) : 0;
                if (len > 0 && buf[0] >= 32 && buf[0] < 127 && !ctrl) {
                    if (app.search_query_len + len < (int)sizeof(app.search_query) - 1) {
                        memcpy(app.search_query + app.search_query_len, buf, len);
                        app.search_query_len += len;
                        app.search_query[app.search_query_len] = '\0';
                        update_search_matches();
                    }
                    app.held_keycode = keycode;
                    app.held_bytes_len = len;
                    memcpy(app.held_bytes, buf, len);
                    app.next_repeat_time_ms = get_time_ms() + (app.repeat_delay_ms > 0 ? app.repeat_delay_ms : 250);
                    app.is_repeating = 0;
                }
                return;
            }
        }

        if ((keycode == 36 || sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && shift && !app.is_altscreen) {
            write(app.pty_master, "\\\n", 2);
            app.scrollback_offset = 0;
            app.needs_render = 1;
            app.held_keycode = keycode;
            app.held_bytes_len = 2;
            memcpy(app.held_bytes, "\\\n", 2);
            app.next_repeat_time_ms = get_time_ms() + (app.repeat_delay_ms > 0 ? app.repeat_delay_ms : 250);
            app.is_repeating = 0;
            return;
        }

        if (alt && (sym == XKB_KEY_slash || sym == XKB_KEY_question) && !app.is_altscreen) {
            app.search_active = 1;
            app.search_query[0] = '\0';
            app.search_query_len = 0;
            update_terminal_size_for_search();
            update_search_matches();
            return;
        }

        if (ctrl && !alt && !shift && (sym == XKB_KEY_l || sym == XKB_KEY_L) && !app.is_altscreen) {
            clear_full_history();
            write(app.pty_master, "\x0c", 1);
            return;
        }

        if (ctrl && !alt && !shift && (sym == XKB_KEY_c || sym == XKB_KEY_C)) {
            app.scrollback_offset = 0;
            if (app.is_altscreen) {
                write(app.pty_master, "\x03", 1);
            } else {
                pid_t fg_pgrp = tcgetpgrp(app.pty_master);
                if (fg_pgrp > 1) kill(-fg_pgrp, SIGINT);
                else if (app.pid > 0) kill(-app.pid, SIGINT);
                write(app.pty_master, "\x03", 1);
                app.needs_render = 1;
            }
            return;
        }

        if (app.scrollback_offset > 0 && !ctrl && !alt) {
            app.scrollback_offset = 0;
            app.needs_render = 1;
        }

        char buf[32] = {0};
        int len = 0;

        if (keycode == 22 || sym == 65288 || sym == XKB_KEY_BackSpace) {
            buf[0] = ctrl ? 0x17 : 0x7f;
            len = 1;
        } else if (sym == XKB_KEY_Delete || sym == XKB_KEY_KP_Delete) { memcpy(buf, "\033[3~", 4); len = 4; }
        else if (sym == XKB_KEY_Insert || sym == XKB_KEY_KP_Insert) { memcpy(buf, "\033[2~", 4); len = 4; }
        else if (sym == XKB_KEY_Home || sym == XKB_KEY_KP_Home) { memcpy(buf, "\033[H", 3); len = 3; }
        else if (sym == XKB_KEY_End || sym == XKB_KEY_KP_End) { memcpy(buf, "\033[F", 3); len = 3; }
        else if (sym == XKB_KEY_Page_Up || sym == XKB_KEY_KP_Page_Up) { memcpy(buf, "\033[5~", 4); len = 4; }
        else if (sym == XKB_KEY_Page_Down || sym == XKB_KEY_KP_Page_Down) { memcpy(buf, "\033[6~", 4); len = 4; }
        else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { buf[0] = '\r'; len = 1; }
        else if (sym == XKB_KEY_Tab) { buf[0] = '\t'; len = 1; }
        else if (sym == XKB_KEY_Escape) { buf[0] = '\x1b'; len = 1; }
        else if (sym == XKB_KEY_Up) { memcpy(buf, "\033[A", 3); len = 3; }
        else if (sym == XKB_KEY_Down) { memcpy(buf, "\033[B", 3); len = 3; }
        else if (sym == XKB_KEY_Right) {
            if (ctrl) { memcpy(buf, "\033[1;5C", 6); len = 6; }
            else { memcpy(buf, "\033[C", 3); len = 3; }
        } else if (sym == XKB_KEY_Left) {
            if (ctrl) { memcpy(buf, "\033[1;5D", 6); len = 6; }
            else { memcpy(buf, "\033[D", 3); len = 3; }
        } else if (app.xkb_state) {
            len = xkb_state_key_get_utf8(app.xkb_state, keycode, buf, sizeof(buf));
        }

        if (len > 0) {
            if (alt && len < (int)sizeof(buf) - 1) {
                memmove(buf + 1, buf, len);
                buf[0] = '\033';
                len++;
            }
            write(app.pty_master, buf, len);
            app.held_keycode = keycode;
            app.held_bytes_len = len;
            memcpy(app.held_bytes, buf, len);
            app.next_repeat_time_ms = get_time_ms() + (app.repeat_delay_ms > 0 ? app.repeat_delay_ms : 250);
            app.is_repeating = 0;
        }
    }
}

static void keyboard_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {
    app.held_keycode = 0;
    app.held_bytes_len = 0;
    app.active_bind_keycode = 0;
}

static void keyboard_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *k2) { app.needs_render = 1; }
static void keyboard_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t md, uint32_t ml, uint32_t mk, uint32_t g) {
    if (app.xkb_state) xkb_state_update_mask(app.xkb_state, md, ml, mk, 0, 0, g);
}
static void keyboard_repeat_info(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    app.repeat_rate_hz = rate > 0 ? rate : 30;
    app.repeat_delay_ms = delay > 0 ? delay : 250;
}

static const struct wl_keyboard_listener keyboard_keyboard_listener = {
    .keymap       = keyboard_keymap,
    .enter        = keyboard_enter,
    .leave        = keyboard_leave,
    .key          = keyboard_key,
    .modifiers    = keyboard_modifiers,
    .repeat_info  = keyboard_repeat_info,
};

static void seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !app.keyboard) {
        app.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app.keyboard, &keyboard_keyboard_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app.pointer) {
        app.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app.pointer, &pointer_listener, NULL);
        app.cursor_theme = wl_cursor_theme_load(NULL, 24, app.shm);
        if (app.cursor_theme) {
            app.cursor_text = wl_cursor_theme_get_cursor(app.cursor_theme, "xterm");
            if (!app.cursor_text) app.cursor_text = wl_cursor_theme_get_cursor(app.cursor_theme, "text");
            app.cursor_default = wl_cursor_theme_get_cursor(app.cursor_theme, "left_ptr");
        }
    }
}

static void seat_handle_name(void *data, struct wl_seat *seat, const char *name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name         = seat_handle_name,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t id, const char *interface, uint32_t ver) {
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app.compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app.shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app.xdg_wm_base = wl_registry_bind(reg, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(app.xdg_wm_base, &xdg_wm_base_listener, NULL);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        app.seat = wl_registry_bind(reg, id, &wl_seat_interface, 5);
        wl_seat_add_listener(app.seat, &seat_listener, NULL);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t id) {}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "c:e:")) != -1) {
        switch (opt) {
            case 'c':
            case 'e':
                app.execute_cmd = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s [-c command] [-e command]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    setlocale(LC_ALL, "");
    load_config();

    app.inotify_fd = inotify_init1(IN_NONBLOCK);
    if (app.inotify_fd >= 0) {
        char path[1024];
        const char *home = getenv("HOME");
        snprintf(path, sizeof(path), "%s/.config/l", home ? home : "/root");
        mkdir(path, 0755);
        app.inotify_wd = inotify_add_watch(app.inotify_fd, path, IN_CLOSE_WRITE | IN_MOVED_TO);
    }

    app.width = DEFAULT_WIDTH;
    app.height = DEFAULT_HEIGHT;

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        fprintf(stderr, "Error: failed to connect to Wayland compositor\n");
        return 1;
    }

    app.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, NULL);
    wl_display_roundtrip(app.display);

    if (!app.compositor || !app.shm || !app.xdg_wm_base) {
        fprintf(stderr, "Error: required Wayland interfaces not found\n");
        return 1;
    }

    create_buffer();

    int pr = get_padding_right();
    int calc_cols = (DEFAULT_WIDTH - app.padding_x - pr) / (app.font_width > 0 ? app.font_width : 8);
    int calc_rows = (DEFAULT_HEIGHT - (app.padding_y * 2)) / (app.font_height > 0 ? app.font_height : 16);
    app.cols = calc_cols < MIN_VTERM_COLS ? MIN_VTERM_COLS : calc_cols;
    app.rows = calc_rows < MIN_VTERM_ROWS ? MIN_VTERM_ROWS : calc_rows;

    app.vterm = vterm_new(app.rows, app.cols);
    vterm_set_utf8(app.vterm, 1);

    VTermState *state = vterm_obtain_state(app.vterm);
    vterm_state_set_bold_highbright(state, 1);

    VTermScreen *vtscreen = vterm_obtain_screen(app.vterm);
    vterm_screen_enable_altscreen(vtscreen, 1);

    app.vtscreen = vtscreen;
    vterm_screen_reset(app.vtscreen, 1);

    vterm_output_set_callback(app.vterm, term_output, NULL);
    vterm_screen_set_callbacks(app.vtscreen, &vt_callbacks, NULL);

    app.surface = wl_compositor_create_surface(app.compositor);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.xdg_wm_base, app.surface);
    xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, NULL);

    app.xdg_toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    xdg_toplevel_add_listener(app.xdg_toplevel, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(app.xdg_toplevel, "l");
    xdg_toplevel_set_app_id(app.xdg_toplevel, "l");

    wl_surface_commit(app.surface);
    wl_display_roundtrip(app.display);

    while (!app.configured) wl_display_dispatch(app.display);

    app.frame_callback = wl_surface_frame(app.surface);
    wl_callback_add_listener(app.frame_callback, &frame_listener, NULL);
    wl_surface_commit(app.surface);

    struct termios term_attr;
    tcgetattr(STDIN_FILENO, &term_attr);
    term_attr.c_iflag |= (ICRNL | IXON);
    term_attr.c_oflag |= (OPOST | ONLCR);
    term_attr.c_lflag |= (ECHO | ICANON | ISIG | IEXTEN);
    term_attr.c_cc[VERASE] = 0x7f;

    app.pid = forkpty(&app.pty_master, NULL, &term_attr, NULL);
    if (app.pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        const char *sh = app.shell ? app.shell : "fish";
        if (app.execute_cmd) {
            execlp(sh, sh, "-c", app.execute_cmd, NULL);
            execlp("/bin/sh", "sh", "-c", app.execute_cmd, NULL);
        } else {
            execlp(sh, sh, NULL);
            execlp("/bin/sh", "sh", NULL);
        }
        _exit(1);
    }

    resize_pty();

    int wl_fd = wl_display_get_fd(app.display);
    fd_set fds;

    while (1) {
        wl_display_dispatch_pending(app.display);
        wl_display_flush(app.display);

        if (app.held_keycode != 0) {
            long long now = get_time_ms();
            if (now >= app.next_repeat_time_ms) {
                if (app.search_active && !app.is_altscreen) {
                    int ctrl = app.xkb_state ? xkb_state_mod_name_is_active(app.xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) : 0;
                    xkb_keysym_t sym = app.xkb_state ? xkb_state_key_get_one_sym(app.xkb_state, app.held_keycode) : XKB_KEY_NoSymbol;

                    if ((ctrl && (sym == XKB_KEY_w || sym == XKB_KEY_W)) || app.held_keycode == 22 || sym == 65288 || sym == XKB_KEY_BackSpace) {
                        if (app.search_query_len > 0) {
                            if (ctrl && (sym == XKB_KEY_w || sym == XKB_KEY_W || sym == 65288 || app.held_keycode == 22)) {
                                while (app.search_query_len > 0 && app.search_query[app.search_query_len - 1] == ' ') app.search_query[--app.search_query_len] = '\0';
                                while (app.search_query_len > 0 && app.search_query[app.search_query_len - 1] != ' ') app.search_query[--app.search_query_len] = '\0';
                            } else {
                                app.search_query[--app.search_query_len] = '\0';
                            }
                            update_search_matches();
                        }
                    } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
                        if (app.search_matches_count > 0) {
                            int shift = app.xkb_state ? xkb_state_mod_name_is_active(app.xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) : 0;
                            if (shift) {
                                app.search_current_match--;
                                if (app.search_current_match < 0) app.search_current_match = app.search_matches_count - 1;
                            } else {
                                app.search_current_match++;
                                if (app.search_current_match >= app.search_matches_count) app.search_current_match = 0;
                            }
                            int screen_row = app.search_matches[app.search_current_match].row - app.scrollback_count;
                            if (screen_row < 0) {
                                app.scrollback_offset = -screen_row;
                                if (app.scrollback_offset > app.scrollback_count) app.scrollback_offset = app.scrollback_count;
                            } else if (screen_row >= app.rows) {
                                int shift_val = screen_row - (app.rows - 1);
                                if (shift_val < 1) shift_val = 1;
                                app.scrollback_offset += shift_val;
                                if (app.scrollback_offset > app.scrollback_count) app.scrollback_offset = app.scrollback_count;
                            } else {
                                app.scrollback_offset = 0;
                            }
                            app.needs_render = 1;
                        }
                    } else if (app.held_bytes_len > 0 && !ctrl) {
                        if (app.search_query_len + app.held_bytes_len < (int)sizeof(app.search_query) - 1) {
                            memcpy(app.search_query + app.search_query_len, app.held_bytes, app.held_bytes_len);
                            app.search_query_len += app.held_bytes_len;
                            app.search_query[app.search_query_len] = '\0';
                            update_search_matches();
                        }
                    }
                } else {
                    write(app.pty_master, app.held_bytes, app.held_bytes_len);
                }
                app.next_repeat_time_ms = now + (1000 / (app.repeat_rate_hz > 0 ? app.repeat_rate_hz : 30));
            }
        }

        FD_ZERO(&fds);
        FD_SET(wl_fd, &fds);
        FD_SET(app.pty_master, &fds);
        if (app.inotify_fd >= 0) FD_SET(app.inotify_fd, &fds);

        int max_fd = wl_fd;
        if (app.pty_master > max_fd) max_fd = app.pty_master;
        if (app.inotify_fd >= 0 && app.inotify_fd > max_fd) max_fd = app.inotify_fd;

        struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
        if (select(max_fd + 1, &fds, NULL, NULL, &tv) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (app.inotify_fd >= 0 && FD_ISSET(app.inotify_fd, &fds)) {
            char event_buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
            ssize_t len = read(app.inotify_fd, event_buf, sizeof(event_buf));
            if (len > 0) {
                for (char *ptr = event_buf; ptr < event_buf + len; ) {
                    struct inotify_event *event = (struct inotify_event *)ptr;
                    if (event->len > 0 && strcmp(event->name, "config.json") == 0) {
                        load_config();
                        create_buffer();
                        resize_pty();
                        app.needs_render = 1;
                        render();
                    }
                    ptr += sizeof(struct inotify_event) + event->len;
                }
            }
        }

        if (FD_ISSET(app.pty_master, &fds)) {
            char buf[8192];
            int n = read(app.pty_master, buf, sizeof(buf));
            if (n > 0) {
                if (strstr(buf, "\033[?1000h") || strstr(buf, "\033[?1002h") || strstr(buf, "\033[?1006h")) app.mouse_reporting = 1;
                if (strstr(buf, "\033[?1000l") || strstr(buf, "\033[?1002l") || strstr(buf, "\033[?1006l")) app.mouse_reporting = 0;
                if (strstr(buf, "\033[?1049h") || strstr(buf, "\033[?47h") || strstr(buf, "\033[?1047h")) {
                    app.is_altscreen = 1;
                    app.scrollback_offset = 0;
                    app.search_active = 0;
                }
                if (strstr(buf, "\033[?1049l") || strstr(buf, "\033[?47l") || strstr(buf, "\033[?1047l")) {
                    app.is_altscreen = 0;
                    app.scrollback_offset = 0;
                }

                vterm_input_write(app.vterm, buf, n);
                if (app.search_active) update_search_matches();
                if (!app.frame_callback && app.configured) {
                    app.frame_callback = wl_surface_frame(app.surface);
                    wl_callback_add_listener(app.frame_callback, &frame_listener, NULL);
                    wl_surface_commit(app.surface);
                }
            } else {
                break;
            }
        }

        if (FD_ISSET(wl_fd, &fds)) {
            if (wl_display_dispatch(app.display) < 0) break;
        }
    }

    vterm_free(app.vterm);
    wl_display_disconnect(app.display);
    return 0;
}
