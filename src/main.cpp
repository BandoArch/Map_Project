/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <Arduino.h>
#include <esp_log.h>
#include "map_storage.h"
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include "tile_map.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

namespace {

const lv_color_t COLOR_BACKGROUND = lv_color_hex(0x101713);
const lv_color_t COLOR_PANEL = lv_color_hex(0x1A2620);
const lv_color_t COLOR_TEXT = lv_color_hex(0xE6EEE8);
const lv_color_t COLOR_MUTED = lv_color_hex(0x93A59A);
const lv_color_t COLOR_ACCENT = lv_color_hex(0xB9E769);
const lv_color_t COLOR_WARNING = lv_color_hex(0xF0B45B);

lv_obj_t *zoom_label;
lv_obj_t *waypoint_label;
lv_obj_t *gps_status_label;
lv_obj_t *map_state_label;
lv_obj_t *map_detail_label;
lv_obj_t *map_heading;
lv_obj_t *gps_detail;
lv_obj_t *attribution_label;
lv_obj_t *layer_button_label;
bool satellite_mode = false;
bool pending_layer_toggle = false;
int zoom_level = 16;
int pending_zoom = -1;
bool tile_upload_active = false;

bool show_map_tile(int zoom);
void check_map_storage();
int waypoint_count = 0;
bool sd_ready = false;

// USB serial test: SDTEST <ASCII payload>\n. Never overwrite existing files.
void handle_sd_upload_test(const char *payload)
{
    if (!sd_ready) {
        Serial.println("SDTEST ERROR SD card is not mounted");
        return;
    }
    const size_t length = strlen(payload);
    if (length == 0 || length > 160) {
        Serial.println("SDTEST ERROR payload must be 1-160 bytes");
        return;
    }
    char path[32];
    bool available = false;
    for (int i = 0; i < 100; ++i) {
        snprintf(path, sizeof(path), "/sdtest_%02d.txt", i);
        if (!map_storage().exists(path)) {
            available = true;
            break;
        }
    }
    if (!available) {
        Serial.println("SDTEST ERROR all test filenames already exist");
        return;
    }
    Serial.printf("SDTEST STEP create %s\n", path);
    File output = map_storage().open(path, FILE_WRITE);
    if (!output) {
        Serial.println("SDTEST ERROR cannot create file");
        return;
    }
    Serial.printf("SDTEST STEP write %u bytes\n", static_cast<unsigned int>(length));
    const size_t written = output.write(reinterpret_cast<const uint8_t *>(payload), length);
    output.flush();
    output.close();
    if (written != length) {
        Serial.println("SDTEST ERROR incomplete write");
        return;
    }
    Serial.println("SDTEST STEP reopen for readback");
    File input = map_storage().open(path, FILE_READ);
    if (!input) {
        Serial.println("SDTEST ERROR cannot reopen file");
        return;
    }
    char readback[161];
    const size_t size = input.size();
    const size_t received = input.readBytes(readback, sizeof(readback) - 1);
    input.close();
    readback[received] = '\0';
    if (size != length || received != length || memcmp(payload, readback, length) != 0) {
        Serial.println("SDTEST ERROR readback mismatch");
        return;
    }
    Serial.printf("SDTEST FILE %s BYTES %u\n", path, static_cast<unsigned int>(received));
    Serial.printf("SDTEST READBACK %s\n", readback);
    Serial.println("SDTEST OK");
}

void poll_serial_test()
{
    static char command[192];
    static size_t used = 0;
    static bool overflow = false;
    while (Serial.available()) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r') continue;
        if (c == '\n') {
            command[used] = '\0';
            if (overflow) {
                Serial.println("SDTEST ERROR command too long");
            } else if (strcmp(command, "SDDIAG") == 0) {
                lvgl_port_lock(-1);
                if (!sd_ready) check_map_storage();
                map_storage_diagnostics();
                if (sd_ready) show_map_tile(zoom_level);
                lvgl_port_unlock();
            } else if (strncmp(command, "SDTEST ", 7) == 0) {
                lvgl_port_lock(-1);
                handle_sd_upload_test(command + 7);
                lvgl_port_unlock();
            } else if (strncmp(command, "TILEPUT ", 8) == 0 || strncmp(command, "OSMPUT ", 7) == 0 || strncmp(command, "SATPUT ", 7) == 0) {
                if (!sd_ready) {
                    Serial.println("TILE ERROR SD card is not mounted");
                } else {
                    lvgl_port_lock(-1);
                    tile_upload_active = true;
                    lvgl_port_unlock();
                    const bool osm_file = command[0] == 'O';
                    const bool satellite_file = command[0] == 'S';
                    const int zoom = tile_map_receive(command + ((osm_file || satellite_file) ? 7 : 8), osm_file, satellite_file);
                    lvgl_port_lock(-1);
                    tile_upload_active = false;
                    if (zoom >= 0) {
                        tile_map_clear_cache();
                        if (satellite_file) satellite_mode = true;
                        tile_map_set_satellite(satellite_mode);
                        show_map_tile(satellite_file ? zoom_level : zoom);
                    }
                    lvgl_port_unlock();
                }
            }
            used = 0;
            overflow = false;
        } else if (used < sizeof(command) - 1) {
            command[used++] = c;
        } else {
            overflow = true;
        }
    }
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 5, 0);
    lv_obj_set_style_bg_color(button, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xD1F18F), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0x152015), 0);
    lv_obj_center(label);
    return button;
}

void on_zoom(lv_event_t *event)
{
    if (tile_upload_active) return;
    const int change = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    // Rendering needs more stack than the LVGL event task can provide.
    pending_zoom = constrain((pending_zoom >= 0 ? pending_zoom : zoom_level) + change, 14, 18);
}

void on_layer_toggle(lv_event_t *)
{
    if (!tile_upload_active) pending_layer_toggle = !pending_layer_toggle;
}

void on_map_drag(lv_event_t *event)
{
    static lv_point_t previous;
    static bool dragging = false;
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (dragging) Serial.println("MAP PAN finished");
        dragging = false;
        return;
    }
    if (tile_upload_active || !sd_ready) {
        dragging = false;
        return;
    }
    lv_indev_t *input = lv_indev_get_act();
    if (!input) return;
    lv_point_t point;
    lv_indev_get_point(input, &point);
    if (code == LV_EVENT_PRESSED) {
        previous = point;
        dragging = true;
    } else if (code == LV_EVENT_PRESSING && dragging) {
        tile_map_pan(point.x - previous.x, point.y - previous.y);
        previous = point;
    }
}

void poll_map_zoom()
{
    lvgl_port_lock(-1);
    if (!tile_upload_active && pending_layer_toggle) {
        pending_layer_toggle = false;
        satellite_mode = !satellite_mode;
        tile_map_set_satellite(satellite_mode);
        lv_label_set_text(layer_button_label, satellite_mode ? "MAP" : "SAT");
        if (!show_map_tile(zoom_level)) {
            lv_label_set_text(map_state_label, satellite_mode ? "SATELLITE UNAVAILABLE" : "MAP UNAVAILABLE");
            lv_obj_clear_flag(map_state_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!tile_upload_active && pending_zoom >= 0) {
        const int requested = pending_zoom;
        pending_zoom = -1;
        if (requested != zoom_level) {
            Serial.printf("MAP ZOOM requested=%d\n", requested);
            if (!show_map_tile(requested)) {
                lv_label_set_text(map_state_label, "MAP VIEW UNAVAILABLE");
                lv_obj_clear_flag(map_state_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    lvgl_port_unlock();
}

bool show_map_tile(int zoom)
{
    if (!sd_ready) return false;
    if (!tile_map_load(zoom)) {
        if (!map_storage_check()) {
            sd_ready = false;
            tile_map_clear_cache();
            map_storage_end();
        }
        return false;
    }
    zoom_level = zoom;
    lv_label_set_text_fmt(zoom_label, "VIEW %02d", zoom_level);
    lv_label_set_text(map_heading, satellite_mode ? "SAT / 2016" : "SD / OSM MAP");
    lv_label_set_text(layer_button_label, satellite_mode ? "MAP" : "SAT");
    lv_label_set_text(attribution_label, satellite_mode ? "EOX s2maps.eu / 2016 / CC BY 4.0" : "(c) OpenStreetMap contributors");
    lv_obj_add_flag(map_state_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(map_detail_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(gps_detail, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void on_mark_waypoint(lv_event_t *)
{
    ++waypoint_count;
    lv_label_set_text_fmt(waypoint_label, "WP %02d RAM", waypoint_count);
}

void create_navigation_screen()
{
    lv_obj_t *screen = lv_scr_act();
    const lv_coord_t screen_width = lv_disp_get_hor_res(nullptr);
    const lv_coord_t screen_height = lv_disp_get_ver_res(nullptr);
    const lv_coord_t margin = 10;
    const lv_coord_t map_top = 43;
    const lv_coord_t footer_height = 45;
    const lv_coord_t map_height = screen_height - map_top - footer_height - 8;
    const lv_coord_t map_width = screen_width - margin * 2;

    lv_obj_set_style_bg_color(screen, COLOR_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *title = make_label(screen, "TRAIL / MAP", &lv_font_montserrat_18, COLOR_TEXT);
    lv_obj_set_pos(title, margin, 5);

    lv_obj_t *subtitle = make_label(screen, "THIKA / OFFLINE TEST", &lv_font_montserrat_10, COLOR_MUTED);
    lv_obj_set_pos(subtitle, margin + 1, 26);

    gps_status_label = make_label(screen, "NO GPS", &lv_font_montserrat_12, COLOR_WARNING);
    lv_obj_set_width(gps_status_label, 94);
    lv_label_set_long_mode(gps_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(gps_status_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(gps_status_label, LV_ALIGN_TOP_RIGHT, -margin, 12);

    lv_obj_t *map_panel = lv_obj_create(screen);
    lv_obj_set_pos(map_panel, margin, map_top);
    lv_obj_set_size(map_panel, map_width, map_height);
    lv_obj_set_style_bg_color(map_panel, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(map_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(map_panel, lv_color_hex(0x314239), 0);
    lv_obj_set_style_border_width(map_panel, 1, 0);
    lv_obj_set_style_radius(map_panel, 5, 0);
    lv_obj_set_style_pad_all(map_panel, 0, 0);
    lv_obj_clear_flag(map_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(map_panel, on_map_drag, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(map_panel, on_map_drag, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(map_panel, on_map_drag, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(map_panel, on_map_drag, LV_EVENT_PRESS_LOST, nullptr);

    // Create image first so labels and controls are drawn above it.
    tile_map_create(map_panel);
    map_heading = make_label(map_panel, "OFFLINE MAP", &lv_font_montserrat_14, COLOR_TEXT);
    lv_obj_align(map_heading, LV_ALIGN_TOP_LEFT, 9, 8);

    map_state_label = make_label(map_panel, "CHECKING SD CARD", &lv_font_montserrat_16, COLOR_WARNING);
    lv_obj_set_style_text_align(map_state_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(map_state_label, LV_ALIGN_CENTER, 0, -12);

    map_detail_label = make_label(map_panel, "Looking for /map.osm", &lv_font_montserrat_12, COLOR_MUTED);
    lv_obj_set_style_text_align(map_detail_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(map_detail_label, LV_ALIGN_CENTER, 0, 10);

    gps_detail = make_label(map_panel, "GPS not implemented",  &lv_font_montserrat_12, COLOR_MUTED);
    lv_obj_set_style_text_align(gps_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(gps_detail, LV_ALIGN_CENTER, 0, 29);

    lv_obj_t *zoom_out = make_button(map_panel, "-", 34, 34);
    lv_obj_align(zoom_out, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_add_event_cb(zoom_out, on_zoom, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(-1)));

    lv_obj_t *zoom_in = make_button(map_panel, "+", 34, 34);
    lv_obj_align(zoom_in, LV_ALIGN_TOP_RIGHT, -8, 48);
    lv_obj_add_event_cb(zoom_in, on_zoom, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(1)));

    zoom_label = make_label(map_panel, "ZOOM 15", &lv_font_montserrat_10, COLOR_MUTED);
    lv_obj_align(zoom_label, LV_ALIGN_TOP_RIGHT, -8, 86);

    // Dark label backgrounds keep text readable on light map tiles.
    for (lv_obj_t *label : {map_heading, zoom_label, map_state_label, map_detail_label}) {
        lv_obj_set_style_bg_color(label, COLOR_PANEL, 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(label, 2, 0);
    }

    lv_obj_t *mark_button = make_button(map_panel, "MARK WAYPOINT", 112, 30);
    lv_obj_align(mark_button, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_add_event_cb(mark_button, on_mark_waypoint, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *layer_button = make_button(map_panel, "SAT", 48, 30);
    lv_obj_align(layer_button, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    layer_button_label = lv_obj_get_child(layer_button, 0);
    lv_obj_add_event_cb(layer_button, on_layer_toggle, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *coordinates = make_label(screen, "LAT --.------  LON --.------", &lv_font_montserrat_12, COLOR_TEXT);
    lv_obj_set_pos(coordinates, margin + 1, screen_height - footer_height + 1);

    waypoint_label = make_label(screen, "WP 00 RAM", &lv_font_montserrat_10, COLOR_MUTED);
    lv_obj_set_width(waypoint_label, 82);
    lv_label_set_long_mode(waypoint_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(waypoint_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(waypoint_label, LV_ALIGN_BOTTOM_RIGHT, -margin, -20);

    lv_obj_t *storage_state = make_label(screen, "(c) OpenStreetMap contributors", &lv_font_montserrat_10, COLOR_MUTED);
    attribution_label = storage_state;
    lv_obj_set_width(storage_state, screen_width - margin * 2);
    lv_label_set_long_mode(storage_state, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(storage_state, margin + 1, screen_height - 19);
}

void check_map_storage()
{
    sd_ready = false;
    Serial.println("Initializing SD card");
    if (!map_storage_begin()) {
        Serial.println("SD card mount failed");
        lv_label_set_text(map_state_label, "SD CARD NOT FOUND");
        lv_label_set_text(map_detail_label, "Insert SD card; retrying automatically");
        return;
    }

    File map_file = map_storage().open("/map.osm", FILE_READ);
    if (!map_file) {
        Serial.println("Card mounted, but /map.osm was not found");
        lv_label_set_text(map_state_label, "MAP FILE NOT FOUND");
        lv_label_set_text(map_detail_label, "Expected /map.osm at card root");
        return;
    }

    char header[64];
    const size_t bytes_read = map_file.readBytes(header, sizeof(header) - 1);
    header[bytes_read] = '\0';
    const unsigned int file_size_kb = static_cast<unsigned int>((map_file.size() + 1023) / 1024);
    Serial.printf("Opened /map.osm: %u bytes read, %u KB total\n",
                  static_cast<unsigned int>(bytes_read), file_size_kb);
    Serial.printf("Map header: %.63s\n", header);
    map_file.close();

    if (bytes_read == 0) {
        lv_label_set_text(map_state_label, "MAP FILE EMPTY");
        lv_label_set_text(map_detail_label, "Could not read /map.osm");
        return;
    }

    sd_ready = true;
    lv_label_set_text(map_state_label, "OSM FILE READY");
    lv_label_set_text_fmt(map_detail_label, "%u KB  /sd/map.osm", file_size_kb);
}

void poll_map_storage()
{
    static uint32_t last_check = 0;
    const uint32_t now = millis();
    if (now - last_check < 3000) return;

    // Serialize card recovery and cached image changes with the UI.
    lvgl_port_lock(-1);
    if (!tile_upload_active) {
        if (sd_ready && !map_storage_check()) {
            Serial.println("STORAGE card removed or unreadable; unmounting");
            sd_ready = false;
            pending_zoom = -1;
            tile_map_clear_cache();
            map_storage_end();
            lv_label_set_text(map_state_label, "SD CARD NOT FOUND");
            lv_label_set_text(map_detail_label, "Insert SD card; retrying automatically");
            lv_obj_clear_flag(map_state_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(map_detail_label, LV_OBJ_FLAG_HIDDEN);
        } else if (!sd_ready) {
            check_map_storage();
            if (!sd_ready) map_storage_end();
            if (sd_ready && !show_map_tile(zoom_level)) {
                lv_label_set_text(map_state_label, "MAP READ FAILED");
                lv_label_set_text(map_detail_label, "Retrying SD card automatically");
            }
            if (!sd_ready) {
                lv_obj_clear_flag(map_state_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(map_detail_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    last_check = millis();
    lvgl_port_unlock();
}

}

void setup()
{
    Serial.begin(115200);
    Serial.setTxTimeoutMs(1000);
    Serial.setDebugOutput(true);
    // Native USB must receive the driver errors as well as application messages.
    esp_log_set_vprintf([](const char *format, va_list args) {
        return static_cast<int>(Serial.vprintf(format, args));
    });

    Serial.println("Initializing board");
    Board *board = new Board();
    board->init();
#if LVGL_PORT_AVOID_TEARING_MODE
    auto lcd = board->getLCD();
    lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
    auto lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
    }
#endif
#endif
    assert(board->begin());

    Serial.println("Initializing LVGL");
    lvgl_port_init(board->getLCD(), board->getTouch());

    Serial.println("Creating navigation UI");
    lvgl_port_lock(-1);
    create_navigation_screen();
    lvgl_port_unlock();

    lvgl_port_lock(-1);
    check_map_storage();
    if (sd_ready && !show_map_tile(zoom_level)) {
        lv_label_set_text(map_state_label, "MAP DATA UNAVAILABLE");
        lv_label_set_text(map_detail_label, "Check /map.osm geometry");
    }
    lvgl_port_unlock();
}

void loop()
{
    poll_serial_test();
    poll_map_zoom();
    poll_map_storage();
    delay(10);
}
