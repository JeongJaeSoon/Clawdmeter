#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <string.h>
#include "logo.h"
#include "icons.h"
#include "codex_icon.h"
#include "hal/board_caps.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_icon_size;
    int16_t usage_metric_y;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    const lv_font_t* usage_title_font;
    const lv_font_t* usage_name_font;
    const lv_font_t* usage_pct_font;
    const lv_font_t* usage_label_font;
    const lv_font_t* usage_reset_font;

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_icon_size = 36;
        L.usage_metric_y = 48;
        L.usage_bar_y = 104;
        L.usage_reset_y = 124;
        L.usage_title_font = &font_tiempos_56;
        L.usage_name_font = &font_styrene_24;
        L.usage_pct_font = &font_styrene_48;
        L.usage_label_font = &font_styrene_16;
        L.usage_reset_font = &font_styrene_14;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_icon_size = 30;
        L.usage_metric_y = 42;
        L.usage_bar_y = 88;
        L.usage_reset_y = 105;
        L.usage_title_font = &font_tiempos_34;
        L.usage_name_font = &font_styrene_20;
        L.usage_pct_font = &font_styrene_28;
        L.usage_label_font = &font_styrene_14;
        L.usage_reset_font = &font_styrene_12;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets ----
static lv_obj_t* usage_dual_container;
static lv_obj_t* usage_claude_container;
static lv_obj_t* usage_codex_container;
static lv_obj_t* lbl_anim;
static lv_obj_t* lbl_anim_claude;
static lv_obj_t* lbl_anim_codex;
static lv_obj_t* pair_groups[3];
static lv_obj_t* idle_groups[3];

struct ProviderUsageWidgets {
    lv_obj_t* panel;
    lv_obj_t* mark;
    lv_obj_t* name;
    lv_obj_t* status_dot;
    lv_obj_t* status;
    lv_obj_t* session_label;
    lv_obj_t* session_pct;
    lv_obj_t* session_bar;
    lv_obj_t* session_reset;
    lv_obj_t* weekly_label;
    lv_obj_t* weekly_pct;
    lv_obj_t* weekly_bar;
    lv_obj_t* weekly_reset;
};
static ProviderUsageWidgets dual_widgets[USAGE_PROVIDER_COUNT];

struct SingleProviderUsageWidgets {
    lv_obj_t* root;
    lv_obj_t* session_panel;
    lv_obj_t* weekly_panel;
    lv_obj_t* session_pct;
    lv_obj_t* session_bar;
    lv_obj_t* session_reset;
    lv_obj_t* weekly_pct;
    lv_obj_t* weekly_bar;
    lv_obj_t* weekly_reset;
};
static SingleProviderUsageWidgets single_widgets[USAGE_PROVIDER_COUNT];

// ---- Bluetooth screen widgets ----
static lv_obj_t* ble_container;
static lv_obj_t* lbl_ble_status;
static lv_obj_t* lbl_ble_device;
static lv_obj_t* lbl_ble_mac;

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* header_icon_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static lv_image_dsc_t codex_icon_dsc;
static lv_image_dsc_t bluetooth_icon_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;
static uint32_t connected_at_ms = 0;   // when we last entered CONNECTED ("Connected" dwell)
static uint32_t last_data_ms = 0;
static bool     data_received = false;
static int      view_state = -1;  // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_short(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "--");
    } else if (mins < 60) {
        snprintf(buf, len, "%dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "%dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "%dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

static void format_reset_label(const char* reset_short, char* buf, size_t len) {
    if (strcmp(reset_short, "--") == 0) {
        snprintf(buf, len, "--");
    } else {
        snprintf(buf, len, "Resets in %s", reset_short);
    }
}

static void set_header_icon(const lv_image_dsc_t* dsc) {
    if (!header_icon_img) return;
    if (!dsc) {
        lv_obj_add_flag(header_icon_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const int slot = LOGO_WIDTH;
    lv_image_set_src(header_icon_img, dsc);
    lv_obj_set_pos(header_icon_img,
                   L.margin + (slot - (int)dsc->header.w) / 2,
                   L.title_y - 10 + (slot - (int)dsc->header.h) / 2);
    lv_obj_clear_flag(header_icon_img, LV_OBJ_FLAG_HIDDEN);
}

static const lv_image_dsc_t* header_icon_for_screen(screen_t screen) {
    switch (screen) {
    case SCREEN_USAGE_CLAUDE: return &logo_dsc;
    case SCREEN_USAGE_CODEX:  return &codex_icon_dsc;
    case SCREEN_BLUETOOTH:    return &bluetooth_icon_dsc;
    default:                  return nullptr;
    }
}

static const lv_image_dsc_t* provider_icon_for_usage(UsageProvider provider) {
    return provider == USAGE_PROVIDER_CODEX ? &codex_icon_dsc : &logo_dsc;
}

static int provider_icon_source_w(UsageProvider provider) {
    return provider == USAGE_PROVIDER_CODEX ? ICON_CODEX_W : LOGO_WIDTH;
}

static int provider_icon_draw_size(void) {
    const int inset = 4;
    return L.usage_icon_size > inset ? L.usage_icon_size - inset : L.usage_icon_size;
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void ble_reset_click_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc(lv_image_dsc_t* dsc, int w, int h, const uint16_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.stride = w * 2;
    dsc->data = (const uint8_t*)data;
    dsc->data_size = w * h * 2;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

static void init_image_descriptors(void) {
    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_icon_dsc_rgb565a8(&codex_icon_dsc, ICON_CODEX_W, ICON_CODEX_H, icon_codex_data);
    init_icon_dsc(&bluetooth_icon_dsc, ICON_BLUETOOTH_W, ICON_BLUETOOTH_H, icon_bluetooth_data);
    init_battery_icons();
}

// ======== Usage Screen ========

static void style_metric_label(lv_obj_t* lbl, const char* text) {
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.usage_label_font, 0);
    lv_obj_set_style_text_color(lbl, COL_DIM, 0);
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.scr_h >= 460 ? &font_styrene_28 : L.usage_name_font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 16, 0);
    lv_obj_set_style_pad_right(lbl, 16, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

static void make_provider_mark(lv_obj_t* panel, ProviderUsageWidgets* widgets,
                               UsageProvider provider) {
    lv_obj_t* mark = lv_obj_create(panel);
    lv_obj_set_size(mark, L.usage_icon_size, L.usage_icon_size);
    lv_obj_set_pos(mark, 0, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_pad_all(mark, 0, 0);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* img = lv_image_create(mark);
    lv_image_set_src(img, provider_icon_for_usage(provider));
    lv_image_set_scale(img, (uint32_t)provider_icon_draw_size() * 256 /
                            provider_icon_source_w(provider));
    lv_obj_center(img);
    widgets->mark = mark;
}

static void make_provider_usage_panel(lv_obj_t* parent, ProviderUsageWidgets* widgets,
                                      UsageProvider provider, int y, int h,
                                      const char* name) {
    widgets->panel = make_panel(parent, L.margin, y, L.content_w, h);
    const int inner_w = L.content_w - 32;

    make_provider_mark(widgets->panel, widgets, provider);

    const int status_w = L.scr_h >= 460 ? 104 : 84;
    const int status_x = inner_w - status_w;
    const int dot_size = L.scr_h >= 460 ? 7 : 6;
    const int name_x = L.usage_icon_size + 10;
    const int name_w = status_x - name_x - 16;

    widgets->name = lv_label_create(widgets->panel);
    lv_label_set_text(widgets->name, name);
    lv_obj_set_style_text_font(widgets->name, L.usage_name_font, 0);
    lv_obj_set_style_text_color(widgets->name, COL_TEXT, 0);
    lv_obj_set_width(widgets->name, name_w);
    lv_label_set_long_mode(widgets->name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(widgets->name, name_x, 3);

    widgets->status_dot = lv_obj_create(widgets->panel);
    lv_obj_set_size(widgets->status_dot, dot_size, dot_size);
    lv_obj_set_pos(widgets->status_dot, status_x, 11);
    lv_obj_set_style_bg_color(widgets->status_dot, COL_DIM, 0);
    lv_obj_set_style_bg_opa(widgets->status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(widgets->status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(widgets->status_dot, 0, 0);
    lv_obj_set_style_pad_all(widgets->status_dot, 0, 0);
    lv_obj_clear_flag(widgets->status_dot, LV_OBJ_FLAG_SCROLLABLE);

    widgets->status = lv_label_create(widgets->panel);
    lv_label_set_text(widgets->status, "waiting");
    lv_obj_set_style_text_font(widgets->status, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(widgets->status, COL_DIM, 0);
    lv_obj_set_width(widgets->status, status_w - dot_size - 6);
    lv_label_set_long_mode(widgets->status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(widgets->status, status_x + dot_size + 6, 8);

    const int gap = 14;
    const int col_w = (inner_w - gap) / 2;
    const int col2 = col_w + gap;

    widgets->session_label = lv_label_create(widgets->panel);
    style_metric_label(widgets->session_label, "5h Session");
    lv_obj_set_width(widgets->session_label, col_w);
    lv_obj_set_pos(widgets->session_label, 0, L.usage_metric_y);

    widgets->weekly_label = lv_label_create(widgets->panel);
    style_metric_label(widgets->weekly_label, "Weekly");
    lv_obj_set_width(widgets->weekly_label, col_w);
    lv_obj_set_pos(widgets->weekly_label, col2, L.usage_metric_y);

    widgets->session_pct = lv_label_create(widgets->panel);
    lv_label_set_text(widgets->session_pct, "--%");
    lv_obj_set_style_text_font(widgets->session_pct, L.usage_pct_font, 0);
    lv_obj_set_style_text_color(widgets->session_pct, COL_TEXT, 0);
    lv_obj_set_pos(widgets->session_pct, 0, L.usage_metric_y + 14);

    widgets->weekly_pct = lv_label_create(widgets->panel);
    lv_label_set_text(widgets->weekly_pct, "--%");
    lv_obj_set_style_text_font(widgets->weekly_pct, L.usage_pct_font, 0);
    lv_obj_set_style_text_color(widgets->weekly_pct, COL_TEXT, 0);
    lv_obj_set_pos(widgets->weekly_pct, col2, L.usage_metric_y + 14);

    widgets->session_bar = make_bar(widgets->panel, 0, L.usage_bar_y, col_w, 12);
    widgets->weekly_bar = make_bar(widgets->panel, col2, L.usage_bar_y, col_w, 12);

    widgets->session_reset = lv_label_create(widgets->panel);
    lv_label_set_text(widgets->session_reset, "--");
    lv_obj_set_style_text_font(widgets->session_reset, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(widgets->session_reset, COL_DIM, 0);
    lv_obj_set_width(widgets->session_reset, col_w);
    lv_label_set_long_mode(widgets->session_reset, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(widgets->session_reset, 0, L.usage_reset_y);

    widgets->weekly_reset = lv_label_create(widgets->panel);
    lv_label_set_text(widgets->weekly_reset, "--");
    lv_obj_set_style_text_font(widgets->weekly_reset, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(widgets->weekly_reset, COL_DIM, 0);
    lv_obj_set_width(widgets->weekly_reset, col_w);
    lv_label_set_long_mode(widgets->weekly_reset, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(widgets->weekly_reset, col2, L.usage_reset_y);
}

static void make_single_metric_panel(lv_obj_t* parent, int y, const char* label,
                                     lv_obj_t** out_panel,
                                     lv_obj_t** out_pct, lv_obj_t** out_bar,
                                     lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);
    *out_panel = panel;
    const int inner_w = L.content_w - 32;
    const bool large = L.scr_h >= 460;
    const int bar_y = large ? 56 : 50;
    const int bar_h = large ? 24 : 18;
    const int reset_y = large ? 94 : 84;

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "--%");
    lv_obj_set_style_text_font(*out_pct, L.usage_pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    lv_obj_t* pill = make_pill(panel, label);
    lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, bar_y, inner_w, bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "--");
    lv_obj_set_style_text_font(*out_reset, L.scr_h >= 460 ? &font_styrene_28 : L.usage_name_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, reset_y);
}

static lv_obj_t* make_usage_root(lv_obj_t* scr, const char* title) {
    lv_obj_t* root = lv_obj_create(scr);
    lv_obj_set_size(root, L.scr_w, L.scr_h);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_title = lv_label_create(root);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, L.usage_title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, L.title_y);
    return root;
}

static void build_pair_group(lv_obj_t* parent, int idx) {
    lv_obj_t* group = lv_obj_create(parent);
    lv_obj_set_size(group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(group, 0, L.content_y);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(group, 0, 0);
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t* l2 = lv_label_create(group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t* l3 = lv_label_create(group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, 155);

    lv_obj_add_flag(group, LV_OBJ_FLAG_HIDDEN);
    pair_groups[idx] = group;
}

static void build_idle_group(lv_obj_t* parent, int idx) {
    lv_obj_t* group = lv_obj_create(parent);
    lv_obj_set_size(group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(group, 0, L.content_y);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(group, 0, 0);
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* creature = splash_mini_create(group, "expression sleep", 160);
    lv_obj_align(creature, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_add_flag(group, LV_OBJ_FLAG_HIDDEN);
    idle_groups[idx] = group;
}

static void update_view_state(void) {
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
    view_state = v;

    bool show_usage = (v == 2);
    bool show_pair = (v == 0);
    bool show_idle = (v == 1);

    // Dual provider panels (Usage screen)
    for (int i = 0; i < USAGE_PROVIDER_COUNT; i++) {
        lv_obj_t* panel = dual_widgets[i].panel;
        if (panel) {
            if (show_usage) lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
            else            lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Single provider panels (Claude/Codex screens)
    for (int i = 0; i < USAGE_PROVIDER_COUNT; i++) {
        lv_obj_t* sp = single_widgets[i].session_panel;
        lv_obj_t* wp = single_widgets[i].weekly_panel;
        if (show_usage) {
            if (sp) lv_obj_clear_flag(sp, LV_OBJ_FLAG_HIDDEN);
            if (wp) lv_obj_clear_flag(wp, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (sp) lv_obj_add_flag(sp, LV_OBJ_FLAG_HIDDEN);
            if (wp) lv_obj_add_flag(wp, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Pair/idle groups on all three containers
    for (int i = 0; i < 3; i++) {
        if (!pair_groups[i] || !idle_groups[i]) continue;
        lv_obj_add_flag(pair_groups[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(idle_groups[i], LV_OBJ_FLAG_HIDDEN);
        if (show_pair) lv_obj_clear_flag(pair_groups[i], LV_OBJ_FLAG_HIDDEN);
        if (show_idle) lv_obj_clear_flag(idle_groups[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_dual_container = make_usage_root(scr, "Usage");
    make_provider_usage_panel(usage_dual_container, &dual_widgets[USAGE_PROVIDER_CLAUDE],
                              USAGE_PROVIDER_CLAUDE, L.content_y,
                              L.usage_panel_h, "Claude");
    make_provider_usage_panel(usage_dual_container, &dual_widgets[USAGE_PROVIDER_CODEX],
                              USAGE_PROVIDER_CODEX,
                              L.content_y + L.usage_panel_h + L.usage_panel_gap,
                              L.usage_panel_h, "Codex");

    lbl_anim = lv_label_create(usage_dual_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);

    build_pair_group(usage_dual_container, 0);
    build_idle_group(usage_dual_container, 0);

    usage_claude_container = make_usage_root(scr, "Claude");
    single_widgets[USAGE_PROVIDER_CLAUDE].root = usage_claude_container;
    make_single_metric_panel(usage_claude_container, L.content_y, "5h",
                             &single_widgets[USAGE_PROVIDER_CLAUDE].session_panel,
                             &single_widgets[USAGE_PROVIDER_CLAUDE].session_pct,
                             &single_widgets[USAGE_PROVIDER_CLAUDE].session_bar,
                             &single_widgets[USAGE_PROVIDER_CLAUDE].session_reset);
    make_single_metric_panel(usage_claude_container,
                             L.content_y + L.usage_panel_h + L.usage_panel_gap,
                             "Week",
                             &single_widgets[USAGE_PROVIDER_CLAUDE].weekly_panel,
                             &single_widgets[USAGE_PROVIDER_CLAUDE].weekly_pct,
                             &single_widgets[USAGE_PROVIDER_CLAUDE].weekly_bar,
                             &single_widgets[USAGE_PROVIDER_CLAUDE].weekly_reset);

    lbl_anim_claude = lv_label_create(usage_claude_container);
    lv_label_set_text(lbl_anim_claude, "");
    lv_obj_set_style_text_font(lbl_anim_claude, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim_claude, COL_ACCENT, 0);
    lv_obj_align(lbl_anim_claude, LV_ALIGN_BOTTOM_MID, 0, -15);

    build_pair_group(usage_claude_container, 1);
    build_idle_group(usage_claude_container, 1);

    lv_obj_add_flag(usage_claude_container, LV_OBJ_FLAG_HIDDEN);

    usage_codex_container = make_usage_root(scr, "Codex");
    single_widgets[USAGE_PROVIDER_CODEX].root = usage_codex_container;
    make_single_metric_panel(usage_codex_container, L.content_y, "5h",
                             &single_widgets[USAGE_PROVIDER_CODEX].session_panel,
                             &single_widgets[USAGE_PROVIDER_CODEX].session_pct,
                             &single_widgets[USAGE_PROVIDER_CODEX].session_bar,
                             &single_widgets[USAGE_PROVIDER_CODEX].session_reset);
    make_single_metric_panel(usage_codex_container,
                             L.content_y + L.usage_panel_h + L.usage_panel_gap,
                             "Week",
                             &single_widgets[USAGE_PROVIDER_CODEX].weekly_panel,
                             &single_widgets[USAGE_PROVIDER_CODEX].weekly_pct,
                             &single_widgets[USAGE_PROVIDER_CODEX].weekly_bar,
                             &single_widgets[USAGE_PROVIDER_CODEX].weekly_reset);

    lbl_anim_codex = lv_label_create(usage_codex_container);
    lv_label_set_text(lbl_anim_codex, "");
    lv_obj_set_style_text_font(lbl_anim_codex, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim_codex, COL_ACCENT, 0);
    lv_obj_align(lbl_anim_codex, LV_ALIGN_BOTTOM_MID, 0, -15);

    build_pair_group(usage_codex_container, 2);
    build_idle_group(usage_codex_container, 2);

    lv_obj_add_flag(usage_codex_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Bluetooth Screen ========

static void init_bluetooth_screen(lv_obj_t* scr) {
    ble_container = lv_obj_create(scr);
    lv_obj_set_size(ble_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(ble_container, 0, 0);
    lv_obj_set_style_bg_opa(ble_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ble_container, 0, 0);
    lv_obj_set_style_pad_all(ble_container, 0, 0);
    lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ble_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_ble_title = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_title, "Bluetooth");
    lv_obj_set_style_text_font(lbl_ble_title, L.bt_title_font, 0);
    lv_obj_set_style_text_color(lbl_ble_title, COL_TEXT, 0);
    lv_obj_align(lbl_ble_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    lv_obj_t* p_info = make_panel(ble_container, L.margin, L.content_y,
                                  L.content_w, L.bt_info_panel_h);

    lbl_ble_status = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_status, "Initializing...");
    lv_obj_set_style_text_font(lbl_ble_status, L.bt_status_font, 0);
    lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_status, 0, 2);

    lbl_ble_device = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_device, "Device: ---");
    lv_obj_set_style_text_font(lbl_ble_device, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_ble_device, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_device, 0, 64);

    lbl_ble_mac = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_mac, "Address: ---");
    lv_obj_set_style_text_font(lbl_ble_mac, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_ble_mac, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_mac, 0, 100);

    int reset_y = L.content_y + L.bt_info_panel_h + 16;
    lv_obj_t* reset_zone = lv_obj_create(ble_container);
    lv_obj_set_pos(reset_zone, L.margin, reset_y);
    lv_obj_set_size(reset_zone, L.content_w, L.bt_reset_zone_h);
    lv_obj_set_style_bg_color(reset_zone, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(reset_zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(reset_zone, 8, 0);
    lv_obj_set_style_border_width(reset_zone, 0, 0);
    lv_obj_set_style_pad_column(reset_zone, 14, 0);
    lv_obj_set_flex_flow(reset_zone, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reset_zone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(reset_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(reset_zone, ble_reset_click_cb, LV_EVENT_CLICKED, NULL);

    static lv_image_dsc_t icon_trash_dsc;
    init_icon_dsc(&icon_trash_dsc, ICON_TRASH2_W, ICON_TRASH2_H, icon_trash2_data);
    lv_obj_t* trash_img = lv_image_create(reset_zone);
    lv_image_set_src(trash_img, &icon_trash_dsc);

    lv_obj_t* reset_lbl = lv_label_create(reset_zone);
    lv_label_set_text(reset_lbl, "Reset Bluetooth");
    lv_obj_set_style_text_font(reset_lbl, L.bt_device_font, 0);
    lv_obj_set_style_text_color(reset_lbl, COL_DIM, 0);

    lv_obj_t* lbl_credit = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit, "Built by @hermannbjorgvin");
    lv_obj_set_style_text_font(lbl_credit, L.bt_credit_1_font, 0);
    lv_obj_set_style_text_color(lbl_credit, COL_DIM, 0);
    lv_obj_align(lbl_credit, LV_ALIGN_BOTTOM_MID, 0, -46);

    lv_obj_t* lbl_credit2 = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit2, "Clawd animation by @amaanbuilds");
    lv_obj_set_style_text_font(lbl_credit2, L.bt_credit_2_font, 0);
    lv_obj_set_style_text_color(lbl_credit2, COL_DIM, 0);
    lv_obj_align(lbl_credit2, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_image_descriptors();

    init_usage_screen(scr);
    init_bluetooth_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    header_icon_img = lv_image_create(scr);
    lv_image_set_src(header_icon_img, &logo_dsc);
    lv_obj_set_pos(header_icon_img, L.margin, L.title_y - 10);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);
}

static void set_usage_bar(lv_obj_t* bar, int pct, lv_color_t color) {
    lv_bar_set_value(bar, pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);
}

static void clear_dual_provider_widgets(ProviderUsageWidgets* widgets) {
    lv_label_set_text(widgets->status, "waiting");
    lv_obj_set_style_text_color(widgets->status, COL_DIM, 0);
    lv_obj_set_style_bg_color(widgets->status_dot, COL_DIM, 0);
    lv_label_set_text(widgets->session_pct, "--%");
    lv_label_set_text(widgets->weekly_pct, "--%");
    lv_label_set_text(widgets->session_reset, "--");
    lv_label_set_text(widgets->weekly_reset, "--");
    set_usage_bar(widgets->session_bar, 0, COL_DIM);
    set_usage_bar(widgets->weekly_bar, 0, COL_DIM);
}

static void clear_single_provider_widgets(SingleProviderUsageWidgets* widgets) {
    if (!widgets->root) return;
    lv_label_set_text(widgets->session_pct, "--%");
    lv_label_set_text(widgets->weekly_pct, "--%");
    lv_label_set_text(widgets->session_reset, "--");
    lv_label_set_text(widgets->weekly_reset, "--");
    set_usage_bar(widgets->session_bar, 0, COL_DIM);
    set_usage_bar(widgets->weekly_bar, 0, COL_DIM);
}

static void update_dual_provider_widgets(ProviderUsageWidgets* widgets,
                                         const ProviderUsageData* usage,
                                         int session_pct, int weekly_pct,
                                         const char* session_reset,
                                         const char* weekly_reset) {
    lv_label_set_text_fmt(widgets->session_pct, "%d%%", session_pct);
    lv_label_set_text_fmt(widgets->weekly_pct, "%d%%", weekly_pct);
    set_usage_bar(widgets->session_bar, session_pct, pct_color(usage->session_pct));
    set_usage_bar(widgets->weekly_bar, weekly_pct, pct_color(usage->weekly_pct));
    lv_label_set_text(widgets->session_reset, session_reset);
    lv_label_set_text(widgets->weekly_reset, weekly_reset);
    lv_label_set_text(widgets->status, usage->ok ? usage->status : "error");
    lv_obj_set_style_text_color(widgets->status, usage->ok ? COL_GREEN : COL_RED, 0);
    lv_obj_set_style_bg_color(widgets->status_dot, usage->ok ? COL_GREEN : COL_RED, 0);
}

static void update_single_provider_widgets(SingleProviderUsageWidgets* widgets,
                                           const ProviderUsageData* usage,
                                           int session_pct, int weekly_pct,
                                           const char* session_reset,
                                           const char* weekly_reset) {
    if (!widgets->root) return;
    lv_label_set_text_fmt(widgets->session_pct, "%d%%", session_pct);
    lv_label_set_text_fmt(widgets->weekly_pct, "%d%%", weekly_pct);
    set_usage_bar(widgets->session_bar, session_pct, pct_color(usage->session_pct));
    set_usage_bar(widgets->weekly_bar, weekly_pct, pct_color(usage->weekly_pct));
    lv_label_set_text(widgets->session_reset, session_reset);
    lv_label_set_text(widgets->weekly_reset, weekly_reset);
}

static void update_provider_usage_widgets(ProviderUsageWidgets* dual,
                                          SingleProviderUsageWidgets* single,
                                          const ProviderUsageData* usage) {
    if (!usage->valid) {
        clear_dual_provider_widgets(dual);
        clear_single_provider_widgets(single);
        return;
    }

    const int session_pct = (int)(usage->session_pct + 0.5f);
    const int weekly_pct = (int)(usage->weekly_pct + 0.5f);
    char session_reset[24];
    char weekly_reset[24];
    char session_reset_label[32];
    char weekly_reset_label[32];
    format_reset_short(usage->session_reset_mins, session_reset, sizeof(session_reset));
    format_reset_short(usage->weekly_reset_mins, weekly_reset, sizeof(weekly_reset));
    format_reset_label(session_reset, session_reset_label, sizeof(session_reset_label));
    format_reset_label(weekly_reset, weekly_reset_label, sizeof(weekly_reset_label));

    update_dual_provider_widgets(dual, usage, session_pct, weekly_pct,
                                 session_reset_label, weekly_reset_label);
    update_single_provider_widgets(single, usage, session_pct, weekly_pct,
                                   session_reset_label, weekly_reset_label);
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    last_data_ms = lv_tick_get();
    data_received = true;
    update_view_state();

    for (int i = 0; i < USAGE_PROVIDER_COUNT; i++) {
        update_provider_usage_widgets(&dual_widgets[i], &single_widgets[i],
                                      &data->providers[i]);
    }
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE &&
        current_screen != SCREEN_USAGE_CLAUDE &&
        current_screen != SCREEN_USAGE_CODEX) return;

    update_view_state();

    uint32_t now = lv_tick_get();

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms >= spinner_ms[anim_spinner_idx]) {
        anim_last_ms = now;
        anim_phase = (anim_phase + 1) % SPINNER_PHASES;
        anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                        : (SPINNER_PHASES - anim_phase);

        lv_obj_t* target = lbl_anim;
        if (current_screen == SCREEN_USAGE_CLAUDE) target = lbl_anim_claude;
        if (current_screen == SCREEN_USAGE_CODEX)  target = lbl_anim_codex;

        const char* text;
        if (!s_ble_connected) {
            text = "Waiting";
        } else if (view_state == 1) {
            text = (anim_msg_idx & 1) ? "No data" : "Listening";
        } else if (now - connected_at_ms < 5000) {
            text = "Connected";
        } else {
            text = anim_messages[anim_msg_idx];
        }

        static char buf[80];
        snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
                 spinner_frames[anim_spinner_idx], text);
        lv_label_set_text(target, buf);
    }

    if (view_state == 1) splash_mini_tick();
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

static void ble_reset_click_cb(lv_event_t* e) {
    (void)e;
    ble_clear_bonds();
}

static void hide_screen_roots(void) {
    lv_obj_add_flag(usage_dual_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_claude_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_codex_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();
}

static void show_screen_root(screen_t screen) {
    switch (screen) {
    case SCREEN_SPLASH:       splash_show(); break;
    case SCREEN_USAGE:        lv_obj_clear_flag(usage_dual_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_USAGE_CLAUDE: lv_obj_clear_flag(usage_claude_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_USAGE_CODEX:  lv_obj_clear_flag(usage_codex_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_BLUETOOTH:    lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }
    update_view_state();
}

void ui_show_screen(screen_t screen) {
    hide_screen_roots();
    show_screen_root(screen);
    set_header_icon(header_icon_for_screen(screen));

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_cycle_screen(void) {
    screen_t next;
    switch (current_screen) {
    case SCREEN_USAGE:        next = SCREEN_USAGE_CLAUDE; break;
    case SCREEN_USAGE_CLAUDE: next = SCREEN_USAGE_CODEX;  break;
    case SCREEN_USAGE_CODEX:  next = SCREEN_BLUETOOTH;    break;
    case SCREEN_BLUETOOTH:    next = SCREEN_USAGE;        break;
    default:                  next = SCREEN_USAGE;        break;
    }
    ui_show_screen(next);
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);
    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    update_view_state();

    switch (state) {
    case BLE_STATE_CONNECTED:
        lv_label_set_text(lbl_ble_status, "Connected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_GREEN, 0);
        break;
    case BLE_STATE_ADVERTISING:
        lv_label_set_text(lbl_ble_status, "Advertising...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_AMBER, 0);
        break;
    case BLE_STATE_DISCONNECTED:
        lv_label_set_text(lbl_ble_status, "Disconnected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_RED, 0);
        break;
    default:
        lv_label_set_text(lbl_ble_status, "Initializing...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
        break;
    }

    if (name) {
        static char nbuf[48];
        snprintf(nbuf, sizeof(nbuf), "Device: %s", name);
        lv_label_set_text(lbl_ble_device, nbuf);
    }
    if (mac) {
        static char mbuf[48];
        snprintf(mbuf, sizeof(mbuf), "Address: %s", mac);
        lv_label_set_text(lbl_ble_mac, mbuf);
    }
}

void ui_update_battery(int percent, bool charging) {
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
