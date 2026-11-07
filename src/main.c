#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <pty.h>
#include <locale.h>
#include <errno.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <vterm.h>

#include "xdg-shell-client-protocol.h"
#include "../lib/jsmn.h"

#define DEFAULT_FONT_NAME "DepartureMono"
#define DEFAULT_FONT_SIZE 12
#define DEFAULT_WIDTH 800
#define DEFAULT_HEIGHT 600

#define MAX_CUSTOM_BINDS 64

typedef struct {
    xkb_keysym_t sym;
    uint32_t modifiers;
    char action[64];
} KeyBind;

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

    void *shm_data;
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

    int selecting;
    int sel_start_col, sel_start_row;
    int sel_end_col, sel_end_row;
    double mouse_x, mouse_y;
    int selection_mode; 

    long long last_click_time_ms;
    int last_click_col, last_click_row;

    uint32_t held_keycode;
    char held_bytes[32];
    int held_bytes_len;
    int repeat_delay_ms;
    int repeat_rate_hz;
    long long next_repeat_time_ms;
    int is_repeating;
} Term;

static Term app = {0};
static void render(void);
static void create_buffer(void);
static void resize_pty(void);
static const struct wl_callback_listener frame_listener;

static const double default_palette[16][3] = {
    { 0x5c/255.0, 0x4b/255.0, 0x4b/255.0 },
    { 0xe6/255.0, 0x7e/255.0, 0x80/255.0 },
    { 0x5c/255.0, 0x4b/255.0, 0x50/255.0 },
    { 0xe6/255.0, 0xa0/255.0, 0x80/255.0 },
    { 0xe6/255.0, 0x80/255.0, 0xa0/255.0 },
    { 0xe6/255.0, 0x80/255.0, 0xc0/255.0 },
    { 0xe6/255.0, 0x80/255.0, 0x80/255.0 },
    { 0xe6/255.0, 0xcc/255.0, 0xcc/255.0 },
    { 0x72/255.0, 0x5c/255.0, 0x5c/255.0 },
    { 0xf0/255.0, 0x94/255.0, 0x96/255.0 },
    { 0xf0/255.0, 0xa2/255.0, 0x94/255.0 },
    { 0xf0/255.0, 0xb5/255.0, 0x94/255.0 },
    { 0xf0/255.0, 0x94/255.0, 0xb5/255.0 },
    { 0xf0/255.0, 0x94/255.0, 0xd5/255.0 },
    { 0xf0/255.0, 0x94/255.0, 0x94/255.0 },
    { 0xf5/255.0, 0xe6/255.0, 0xe6/255.0 }
};

static long long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int jsmntyeq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 1;
    }
    return 0;
}

static uint32_t parse_hex_color(const char *str) {
    if (str[0] == '#') str++;
    return (uint32_t)strtol(str, NULL, 16);
}

static void strip_json_comments(char *buf) {
    int in_string = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    
    for (int i = 0; buf[i] != '\0'; i++) {
        if (in_line_comment) {
            if (buf[i] == '\n') in_line_comment = 0;
            buf[i] = ' ';
            continue;
        }
        if (in_block_comment) {
            if (buf[i] == '*' && buf[i+1] == '/') {
                buf[i] = ' ';
                buf[i+1] = ' ';
                in_block_comment = 0;
                i++;
            } else {
                buf[i] = ' ';
            }
            continue;
        }
        if (!in_string) {
            if (buf[i] == '/' && buf[i+1] == '/') {
                buf[i] = ' ';
                buf[i+1] = ' ';
                in_line_comment = 1;
                i++;
                continue;
            }
            if (buf[i] == '/' && buf[i+1] == '*') {
                buf[i] = ' ';
                buf[i+1] = ' ';
                in_block_comment = 1;
                i++;
                continue;
            }
        }
        if (buf[i] == '"' && (i == 0 || buf[i-1] != '\\')) {
            in_string = !in_string;
        }
    }
}

static void create_default_config(const char *path) {
    system("mkdir -p ~/.config/l");
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "{\n");
    fprintf(f, "  \"font_family\": \"%s\",\n", DEFAULT_FONT_NAME);
    fprintf(f, "  \"font_size\": %d,\n", DEFAULT_FONT_SIZE);
    fprintf(f, "  \"bg_color\": \"#282828\",\n");
    fprintf(f, "  \"fg_color\": \"#d3c6aa\",\n");
    fprintf(f, "  \"padding_x\": 6,\n");
    fprintf(f, "  \"padding_y\": 6,\n");
    fprintf(f, "  \"shell\": \"fish\",\n");
    fprintf(f, "  \"binds\": [\n");
    fprintf(f, "    { \"key\": \"c\", \"mods\": \"ctrl+shift\", \"action\": \"copy\" },\n");
    fprintf(f, "    { \"key\": \"equal\", \"mods\": \"ctrl\", \"action\": \"font_increase\" },\n");
    fprintf(f, "    { \"key\": \"minus\", \"mods\": \"ctrl\", \"action\": \"font_decrease\" }\n");
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
}

static void load_config(void) {
    char path[1024];
    const char *home = getenv("HOME");
    snprintf(path, sizeof(path), "%s/.config/l/config.json", home ? home : "/root");

    app.font_family = strdup(DEFAULT_FONT_NAME);
    app.font_size = DEFAULT_FONT_SIZE;
    app.bg_color = 0x282828;
    app.fg_color = 0xd3c6aa;
    app.padding_x = 6;
    app.padding_y = 6;
    app.shell = strdup("fish");
    app.binds_count = 0;
    memcpy(app.palette, default_palette, sizeof(default_palette));

    FILE *f = fopen(path, "r");
    if (!f) {
        create_default_config(path);
        f = fopen(path, "r");
        if (!f) return;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    strip_json_comments(buf);

    jsmn_parser p;
    jsmntok_t t[512];
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
                int token_idx = i + 2;
                for (int b = 0; b < array_size && app.binds_count < MAX_CUSTOM_BINDS; b++) {
                    if (t[token_idx].type == JSMN_OBJECT) {
                        int obj_size = t[token_idx].size;
                        token_idx++;
                        char *k_str = NULL;
                        char *m_str = NULL;
                        char *a_str = NULL;

                        for (int o = 0; o < obj_size; o++) {
                            if (jsmntyeq(buf, &t[token_idx], "key")) {
                                k_str = strndup(buf + t[token_idx+1].start, t[token_idx+1].end - t[token_idx+1].start);
                            } else if (jsmntyeq(buf, &t[token_idx], "mods")) {
                                m_str = strndup(buf + t[token_idx+1].start, t[token_idx+1].end - t[token_idx+1].start);
                            } else if (jsmntyeq(buf, &t[token_idx], "action")) {
                                a_str = strndup(buf + t[token_idx+1].start, t[token_idx+1].end - t[token_idx+1].start);
                            }
                            token_idx += 2;
                        }

                        if (k_str && a_str) {
                            KeyBind *kb = &app.binds[app.binds_count++];
                            kb->sym = xkb_keysym_from_name(k_str, XKB_KEYSYM_NO_FLAGS);
                            kb->modifiers = 0;
                            if (m_str) {
                                if (strstr(m_str, "ctrl")) kb->modifiers |= 1;
                                if (strstr(m_str, "shift")) kb->modifiers |= 2;
                                if (strstr(m_str, "alt")) kb->modifiers |= 4;
                            }
                            snprintf(kb->action, sizeof(kb->action), "%s", a_str);
                        }
                        free(k_str);
                        free(m_str);
                        free(a_str);
                    } else {
                        token_idx += 1;
                    }
                }
                i = token_idx - 1;
            }
        }
    }

    char full_font[128];
    snprintf(full_font, sizeof(full_font), "%s %d", app.font_family, app.font_size);
    app.font_name = strdup(full_font);
    free(buf);
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

static void create_buffer(void) {
    if (app.width <= 0 || app.height <= 0) return;

    int stride = app.width * 4;
    int size = stride * app.height;

    int fd = create_shm_file(size);
    if (fd < 0) exit(EXIT_FAILURE);

    app.shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(app.shm, fd, size);
    app.buffer = wl_shm_pool_create_buffer(pool, 0, app.width, app.height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    app.cairo_surface = cairo_image_surface_create_for_data(app.shm_data, CAIRO_FORMAT_ARGB32,
                                                            app.width, app.height, stride);
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

static void get_cell_colors(VTermScreenCell *cell, double default_r, double default_g, double default_b, 
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

    *out_r = fg_r;
    *out_g = fg_g;
    *out_b = fg_b;
    *out_bg_r = cell_bg_r;
    *out_bg_g = cell_bg_g;
    *out_bg_b = cell_bg_b;
    *has_bg = custom_bg;
}

static void draw_underline(int start_col, int end_col, int row, double r, double g, double b) {
    cairo_set_source_rgb(app.cr, r, g, b);
    cairo_set_line_width(app.cr, 1.2);
    double line_y = (row + 1) * app.font_height - 1.0;
    cairo_move_to(app.cr, start_col * app.font_width, line_y);
    cairo_line_to(app.cr, end_col * app.font_width, line_y);
    cairo_stroke(app.cr);
}

static int is_cell_selected(int r, int c) {
    if (!app.selecting && (app.sel_start_row == app.sel_end_row && app.sel_start_col == app.sel_end_col)) 
        return 0;

    int r1 = app.sel_start_row, c1 = app.sel_start_col;
    int r2 = app.sel_end_row, c2 = app.sel_end_col;

    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        r1 = app.sel_end_row; c1 = app.sel_end_col;
        r2 = app.sel_start_row; c2 = app.sel_start_col;
    }

    if (app.selection_mode == 2) {
        return (r >= r1 && r <= r2);
    }

    if (r > r1 && r < r2) return 1;
    if (r == r1 && r == r2) return (c >= c1 && c <= c2);
    if (r == r1) return (c >= c1);
    if (r == r2) return (c <= c2);

    return 0;
}

static void copy_selection_to_clipboard(void) {
    int r1 = app.sel_start_row, c1 = app.sel_start_col;
    int r2 = app.sel_end_row, c2 = app.sel_end_col;

    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        r1 = app.sel_end_row; c1 = app.sel_end_col;
        r2 = app.sel_start_row; c2 = app.sel_start_col;
    }

    char *text_buf = malloc(1024 * 64);
    if (!text_buf) return;
    text_buf[0] = '\0';
    int offset = 0;

    for (int r = r1; r <= r2; r++) {
        int start_c = (app.selection_mode == 2) ? 0 : ((r == r1) ? c1 : 0);
        int end_c = (app.selection_mode == 2) ? (app.cols - 1) : ((r == r2) ? c2 : (app.cols - 1));

        for (int c = start_c; c <= end_c; c++) {
            VTermScreenCell cell;
            vterm_screen_get_cell(app.vtscreen, (VTermPos){r, c}, &cell);

            char utf8[16] = {0};
            int len = 0;
            if (cell.chars[0] && cell.chars[0] != 0) {
                for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++) {
                    int w = wctomb(utf8 + len, cell.chars[i]);
                    if (w > 0) len += w;
                }
            } else {
                utf8[0] = ' ';
                len = 1;
            }

            if (offset + len < 64 * 1024 - 1) {
                memcpy(text_buf + offset, utf8, len);
                offset += len;
            }
        }
        if (r < r2) {
            text_buf[offset++] = '\n';
        }
    }
    text_buf[offset] = '\0';

    FILE *pipe = popen("wl-copy", "w");
    if (pipe) {
        fwrite(text_buf, 1, offset, pipe);
        pclose(pipe);
    }
    free(text_buf);
}

static void render(void) {
    if (!app.configured || !app.cr || !app.surface || !app.buffer || !app.vtscreen || !app.pango_layout) return;

    double bg_r = ((app.bg_color >> 16) & 0xFF) / 255.0;
    double bg_g = ((app.bg_color >> 8) & 0xFF) / 255.0;
    double bg_b = (app.bg_color & 0xFF) / 255.0;

    cairo_save(app.cr);
    cairo_identity_matrix(app.cr);
    cairo_set_source_rgb(app.cr, bg_r, bg_g, bg_b);
    cairo_paint(app.cr);
    cairo_restore(app.cr);

    cairo_save(app.cr);
    cairo_translate(app.cr, app.padding_x, app.padding_y);

    double default_fg_r = ((app.fg_color >> 16) & 0xFF) / 255.0;
    double default_fg_g = ((app.fg_color >> 8) & 0xFF) / 255.0;
    double default_fg_b = (app.fg_color & 0xFF) / 255.0;

    for (int row = 0; row < app.rows; row++) {
        int col = 0;
        while (col < app.cols) {
            VTermScreenCell first_cell;
            vterm_screen_get_cell(app.vtscreen, (VTermPos){row, col}, &first_cell);

            double cr, cg, cb, cbg_r, cbg_g, cbg_b;
            int has_bg = 0;
            get_cell_colors(&first_cell, default_fg_r, default_fg_g, default_fg_b, bg_r, bg_g, bg_b, &cr, &cg, &cb, &cbg_r, &cbg_g, &cbg_b, &has_bg);

            int is_underline = (first_cell.attrs.underline != 0);
            int selected = is_cell_selected(row, col);

            if ((!first_cell.chars[0] || first_cell.chars[0] == ' ' || first_cell.chars[0] == 0) &&
                first_cell.fg.type != VTERM_COLOR_RGB && first_cell.fg.type != VTERM_COLOR_INDEXED && !has_bg && !is_underline && !selected) {
                col++;
                continue;
            }

            char text_buf[4096] = {0};
            int text_len = 0;
            int start_col = col;
            double cell_r = cr, cell_g = cg, cell_b = cb;
            double cell_bg_r = cbg_r, cell_bg_g = cbg_g, cell_bg_b = cbg_b;
            int cell_has_bg = has_bg;
            int cell_underline = is_underline;
            int cell_selected = selected;

            while (col < app.cols) {
                VTermScreenCell cell;
                vterm_screen_get_cell(app.vtscreen, (VTermPos){row, col}, &cell);

                double ncr, ncg, ncb, ncbg_r, ncbg_g, ncbg_b;
                int nhas_bg = 0;
                get_cell_colors(&cell, default_fg_r, default_fg_g, default_fg_b, bg_r, bg_g, bg_b, &ncr, &ncg, &ncb, &ncbg_r, &ncbg_g, &ncbg_b, &nhas_bg);

                int cur_underline = (cell.attrs.underline != 0);
                int cur_selected = is_cell_selected(row, col);

                if (ncr != cell_r || ncg != cell_g || ncb != cell_b || ncbg_r != cell_bg_r || nhas_bg != cell_has_bg || cur_underline != cell_underline || cur_selected != cell_selected) break;

                char utf8[16] = {0};
                int len = 0;
                if (cell.chars[0] && cell.chars[0] != 0) {
                    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++) {
                        int w = wctomb(utf8 + len, cell.chars[i]);
                        if (w > 0) len += w;
                    }
                } else {
                    utf8[0] = ' ';
                    len = 1;
                }

                if (text_len + len < sizeof(text_buf) - 1) {
                    memcpy(text_buf + text_len, utf8, len);
                    text_len += len;
                }
                col++;
            }

            text_buf[text_len] = '\0';

            if (text_len > 0) {
                if (cell_has_bg || cell_selected) {
                    if (cell_selected) {
                        cairo_set_source_rgba(app.cr, 0.8, 0.8, 0.8, 0.35);
                    } else {
                        cairo_set_source_rgb(app.cr, cell_bg_r, cell_bg_g, cell_bg_b);
                    }
                    cairo_rectangle(app.cr, start_col * app.font_width, row * app.font_height, (col - start_col) * app.font_width, app.font_height);
                    cairo_fill(app.cr);
                }

                cairo_set_source_rgb(app.cr, cell_r, cell_g, cell_b);
                cairo_move_to(app.cr, start_col * app.font_width, row * app.font_height);
                pango_layout_set_text(app.pango_layout, text_buf, text_len);
                pango_cairo_show_layout(app.cr, app.pango_layout);

                if (cell_underline) {
                    draw_underline(start_col, col, row, cell_r, cell_g, cell_b);
                }
            }
        }
    }

    VTermState *state = vterm_obtain_state(app.vterm);
    VTermPos cursor_pos;
    vterm_state_get_cursorpos(state, &cursor_pos);

    VTermScreenCell cursor_cell;
    vterm_screen_get_cell(app.vtscreen, cursor_pos, &cursor_cell);

    double cursor_r = app.palette[1][0];
    double cursor_g = app.palette[1][1];
    double cursor_b = app.palette[1][2];

    double cursor_x = cursor_pos.col * app.font_width;
    double cursor_y = cursor_pos.row * app.font_height;

    cairo_set_source_rgb(app.cr, cursor_r, cursor_g, cursor_b);
    cairo_rectangle(app.cr, cursor_x, cursor_y, (double)app.font_width, app.font_height);
    cairo_fill(app.cr);

    if (cursor_cell.chars[0] && cursor_cell.chars[0] != ' ' && cursor_cell.chars[0] != 0) {
        char ch_buf[16] = {0};
        int ch_len = 0;
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cursor_cell.chars[i]; i++) {
            int w = wctomb(ch_buf + ch_len, cursor_cell.chars[i]);
            if (w > 0) ch_len += w;
        }
        ch_buf[ch_len] = '\0';

        cairo_set_source_rgb(app.cr, bg_r, bg_g, bg_b);
        cairo_move_to(app.cr, cursor_x, cursor_y);
        pango_layout_set_text(app.pango_layout, ch_buf, ch_len);
        pango_cairo_show_layout(app.cr, app.pango_layout);
    }

    cairo_restore(app.cr);

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
    }
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_handle_done,
};

static int term_damage(VTermRect rect, void *user) {
    app.needs_render = 1;
    if (!app.frame_callback) {
        render();
    }
    return 1;
}

static int term_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user) {
    app.needs_render = 1;
    if (!app.frame_callback) {
        render();
    }
    return 1;
}

static void term_output(const char *s, size_t len, void *user) {
    if (app.pty_master > 0) {
        write(app.pty_master, s, len);
    }
}

static VTermScreenCallbacks vt_callbacks = {
    .damage = term_damage,
    .movecursor = term_movecursor,
};

static void resize_pty(void) {
    if (app.pty_master <= 0) return;
    struct winsize ws = {
        .ws_row = app.rows,
        .ws_col = app.cols,
        .ws_xpixel = app.width - (app.padding_x * 2),
        .ws_ypixel = app.height - (app.padding_y * 2)
    };
    ioctl(app.pty_master, TIOCSWINSZ, &ws);
}

static void change_font_size(int delta) {
    app.font_size += delta;
    if (app.font_size < 6) app.font_size = 6;
    if (app.font_size > 72) app.font_size = 72;

    char new_font[128];
    snprintf(new_font, sizeof(new_font), "%s %d", app.font_family, app.font_size);
    
    free(app.font_name);
    app.font_name = strdup(new_font);

    if (app.buffer) {
        cairo_destroy(app.cr);
        g_object_unref(app.pango_layout);
        cairo_surface_destroy(app.cairo_surface);
        munmap(app.shm_data, app.width * app.height * 4);
        wl_buffer_destroy(app.buffer);
        app.buffer = NULL;
    }

    create_buffer();

    app.cols = (app.width - (app.padding_x * 2)) / (app.font_width > 0 ? app.font_width : 8);
    app.rows = (app.height - (app.padding_y * 2)) / (app.font_height > 0 ? app.font_height : 16);
    if (app.cols < 1) app.cols = 1;
    if (app.rows < 1) app.rows = 1;

    if (app.vterm) {
        vterm_set_size(app.vterm, app.rows, app.cols);
    }
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

static void xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *toplevel,
                                           int32_t width, int32_t height, struct wl_array *states) {
    int needs_resize = 0;
    if (width > 0 && height > 0) {
        if (app.width != width || app.height != height) {
            app.width = width;
            app.height = height;
            needs_resize = 1;
        }
    } else {
        if (app.width == 0 || app.height == 0) {
            app.width = DEFAULT_WIDTH;
            app.height = DEFAULT_HEIGHT;
            needs_resize = 1;
        }
    }

    if (needs_resize || !app.buffer) {
        if (app.buffer) {
            cairo_destroy(app.cr);
            g_object_unref(app.pango_layout);
            cairo_surface_destroy(app.cairo_surface);
            munmap(app.shm_data, app.width * app.height * 4);
            wl_buffer_destroy(app.buffer);
            app.buffer = NULL;
        }

        create_buffer();

        app.cols = (app.width - (app.padding_x * 2)) / (app.font_width > 0 ? app.font_width : 8);
        app.rows = (app.height - (app.padding_y * 2)) / (app.font_height > 0 ? app.font_height : 16);
        if (app.cols < 1) app.cols = 1;
        if (app.rows < 1) app.rows = 1;

        if (app.vterm) {
            vterm_set_size(app.vterm, app.rows, app.cols);
        }
        resize_pty();
    }

    app.needs_render = 1;
    render();
}

static void xdg_toplevel_handle_close(void *data, struct xdg_toplevel *toplevel) {
    exit(0);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_handle_configure,
    .close = xdg_toplevel_handle_close,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial) {
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void pointer_handle_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {}
static void pointer_handle_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    app.mouse_x = wl_fixed_to_double(sx);
    app.mouse_y = wl_fixed_to_double(sy);

    if (app.selecting) {
        int col = (app.mouse_x - app.padding_x) / app.font_width;
        int row = (app.mouse_y - app.padding_y) / app.font_height;
        if (col < 0) col = 0;
        if (col >= app.cols) col = app.cols - 1;
        if (row < 0) row = 0;
        if (row >= app.rows) row = app.rows - 1;

        if (app.sel_end_col != col || app.sel_end_row != row) {
            app.sel_end_col = col;
            app.sel_end_row = row;
            app.needs_render = 1;
            if (!app.frame_callback) {
                render();
            }
        }
    }
}

static void pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    if (button == 0x110) {
        int col = (app.mouse_x - app.padding_x) / app.font_width;
        int row = (app.mouse_y - app.padding_y) / app.font_height;
        if (col < 0) col = 0;
        if (col >= app.cols) col = app.cols - 1;
        if (row < 0) row = 0;
        if (row >= app.rows) row = app.rows - 1;

        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            long long now = get_time_ms();
            if ((now - app.last_click_time_ms < 400) && (app.last_click_row == row) && (app.last_click_col == col)) {
                app.selection_mode = 2; 
                app.selecting = 1;
                app.sel_start_col = 0;
                app.sel_start_row = row;
                app.sel_end_col = app.cols - 1;
                app.sel_end_row = row;
            } else {
                app.selection_mode = 0;
                app.selecting = 1;
                app.sel_start_col = col;
                app.sel_start_row = row;
                app.sel_end_col = col;
                app.sel_end_row = row;

                app.last_click_time_ms = now;
                app.last_click_col = col;
                app.last_click_row = row;
            }
        } else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
            app.selecting = 0;
        }
        app.needs_render = 1;
        if (!app.frame_callback) {
            render();
        }
    }
}

static void pointer_handle_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {}
static void pointer_handle_frame(void *data, struct wl_pointer *pointer) {}
static void pointer_handle_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source) {}
static void pointer_handle_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {}
static void pointer_handle_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_handle_enter,
    .leave = pointer_handle_leave,
    .motion = pointer_handle_motion,
    .button = pointer_handle_button,
    .axis = pointer_handle_axis,
    .frame = pointer_handle_frame,
    .axis_source = pointer_handle_axis_source,
    .axis_stop = pointer_handle_axis_stop,
    .axis_discrete = pointer_handle_axis_discrete,
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

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state) {
    uint32_t keycode = key + 8;

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (app.held_keycode == keycode) {
            app.held_keycode = 0;
            app.held_bytes_len = 0;
        }
        return;
    }

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        int ctrl_pressed = xkb_state_mod_name_is_active(app.xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE);
        int shift_pressed = xkb_state_mod_name_is_active(app.xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE);
        int alt_pressed = xkb_state_mod_name_is_active(app.xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE);
        
        uint32_t current_mods = (ctrl_pressed ? 1 : 0) | (shift_pressed ? 2 : 0) | (alt_pressed ? 4 : 0);
        xkb_keysym_t sym = xkb_state_key_get_one_sym(app.xkb_state, keycode);

        for (int i = 0; i < app.binds_count; i++) {
            if (app.binds[i].sym == sym && app.binds[i].modifiers == current_mods) {
                if (strcmp(app.binds[i].action, "copy") == 0) {
                    copy_selection_to_clipboard();
                    return;
                } else if (strcmp(app.binds[i].action, "font_increase") == 0) {
                    change_font_size(1);
                    return;
                } else if (strcmp(app.binds[i].action, "font_decrease") == 0) {
                    change_font_size(-1);
                    return;
                } else {
                    write(app.pty_master, app.binds[i].action, strlen(app.binds[i].action));
                    return;
                }
            }
        }

        char buf[32] = {0};
        int len = 0;

        if (ctrl_pressed && sym == XKB_KEY_BackSpace) {
            buf[0] = 0x17; 
            len = 1;
        } else if (sym == XKB_KEY_BackSpace) {
            buf[0] = 0x7f;
            len = 1;
        } else if (sym == XKB_KEY_Return) {
            buf[0] = '\r';
            len = 1;
        } else if (sym == XKB_KEY_Tab) {
            buf[0] = '\t';
            len = 1;
        } else if (sym == XKB_KEY_Escape) {
            buf[0] = '\x1b';
            len = 1;
        } else if (sym == XKB_KEY_Up) {
            memcpy(buf, "\033[A", 3);
            len = 3;
        } else if (sym == XKB_KEY_Down) {
            memcpy(buf, "\033[B", 3);
            len = 3;
        } else if (sym == XKB_KEY_Right) {
            memcpy(buf, "\033[C", 3);
            len = 3;
        } else if (sym == XKB_KEY_Left) {
            memcpy(buf, "\033[D", 3);
            len = 3;
        } else {
            len = xkb_state_key_get_utf8(app.xkb_state, keycode, buf, sizeof(buf));
        }

        if (len > 0) {
            write(app.pty_master, buf, len);

            app.held_keycode = keycode;
            app.held_bytes_len = len;
            memcpy(app.held_bytes, buf, len);
            long long now = get_time_ms();
            int delay = app.repeat_delay_ms > 0 ? app.repeat_delay_ms : 250;
            app.next_repeat_time_ms = now + delay;
            app.is_repeating = 0;
        }
    }
}

static void keyboard_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *k2) {}
static void keyboard_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {}
static void keyboard_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    if (app.xkb_state) {
        xkb_state_update_mask(app.xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
    }
}
static void keyboard_repeat_info(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    app.repeat_rate_hz = rate > 0 ? rate : 30;
    app.repeat_delay_ms = delay > 0 ? delay : 250;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !app.keyboard) {
        app.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app.keyboard, &keyboard_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app.pointer) {
        app.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app.pointer, &pointer_listener, NULL);
    }
}

static void seat_handle_name(void *data, struct wl_seat *seat, const char *name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
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
    .global = registry_global,
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

    app.cols = (DEFAULT_WIDTH - (app.padding_x * 2)) / 8;
    app.rows = (DEFAULT_HEIGHT - (app.padding_y * 2)) / 16;
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
    xdg_toplevel_set_title(app.xdg_toplevel, "l-terminal");
    xdg_toplevel_set_app_id(app.xdg_toplevel, "l-terminal");

    wl_surface_commit(app.surface);
    wl_display_roundtrip(app.display);

    while (!app.configured) {
        wl_display_dispatch(app.display);
    }

    struct termios term_attr;
    tcgetattr(STDIN_FILENO, &term_attr);
    term_attr.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    app.pid = forkpty(&app.pty_master, NULL, &term_attr, NULL);
    if (app.pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        if (app.execute_cmd) {
            execlp(app.shell, app.shell, "-c", app.execute_cmd, NULL);
        } else {
            execlp(app.shell, app.shell, NULL);
        }
        perror("execlp shell");
        exit(1);
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
                write(app.pty_master, app.held_bytes, app.held_bytes_len);
                int rate = app.repeat_rate_hz > 0 ? app.repeat_rate_hz : 30;
                app.next_repeat_time_ms = now + (1000 / rate);
            }
        }

        FD_ZERO(&fds);
        FD_SET(wl_fd, &fds);
        FD_SET(app.pty_master, &fds);

        int max_fd = (wl_fd > app.pty_master) ? wl_fd : app.pty_master;

        struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };

        if (select(max_fd + 1, &fds, NULL, NULL, &tv) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(app.pty_master, &fds)) {
            char buf[8192];
            int n = read(app.pty_master, buf, sizeof(buf));
            if (n > 0) {
                vterm_input_write(app.vterm, buf, n);
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
