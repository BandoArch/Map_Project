#include "tile_map.h"

#include <Arduino.h>
#include "map_storage.h"
#include "osm_map.h"
#include "map_viewport.h"
#include <esp_heap_caps.h>

namespace {
constexpr size_t PIXEL_BYTES = 256 * 256 * 2;
constexpr size_t TILE_BYTES = 8 + PIXEL_BYTES;
constexpr int ATLAS_SIZE = 1024;
constexpr size_t ATLAS_BYTES = ATLAS_SIZE * ATLAS_SIZE * sizeof(lv_color_t);
constexpr int SAT_SIZE = 512;
constexpr size_t SAT_BYTES = SAT_SIZE * SAT_SIZE * sizeof(lv_color_t);
// A numbered, checksummed stop-and-wait frame keeps host and device aligned.
constexpr size_t CHUNK_BYTES = 128;
constexpr uint32_t RECEIVE_TIMEOUT_MS = 20000;
lv_obj_t *image = nullptr;
lv_img_dsc_t descriptor = {};
lv_color_t *atlas = nullptr;
lv_color_t *satellite_pixels = nullptr;
bool satellite_selected = false;
int display_size = ATLAS_SIZE;
MapViewport viewport;
uint16_t image_scale = 256;
bool using_osm = false;

uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
        }
    }
    return crc;
}

bool valid_header(File &file, bool satellite_file = false)
{
    const uint8_t dimension = satellite_file ? 2 : 1;
    const uint8_t expected[] = {'T', '5', '6', '5', 0, dimension, 0, dimension};
    uint8_t header[8];
    return file.size() == (satellite_file ? SAT_BYTES + 8 : TILE_BYTES) && file.read(header, sizeof(header)) == sizeof(header) &&
           memcmp(header, expected, sizeof(header)) == 0;
}

bool verify_file(const char *path, uint32_t expected_crc, size_t length, bool osm_file, bool satellite_file)
{
    File file = map_storage().open(path, FILE_READ);
    if (!file || file.size() != length) return false;
    if (!osm_file && !valid_header(file, satellite_file)) return false;
    if (osm_file) {
        char prefix[5];
        if (file.read(reinterpret_cast<uint8_t *>(prefix), sizeof(prefix)) != sizeof(prefix) ||
            (memcmp(prefix, "<?xml", 5) && memcmp(prefix, "<osm ", 5))) return false;
    }
    if (!file.seek(0)) return false;
    uint8_t buffer[CHUNK_BYTES];
    uint32_t crc = 0xFFFFFFFFU;
    size_t total = 0;
    while (total < length) {
        const size_t wanted = min(sizeof(buffer), length - total);
        if (file.read(buffer, wanted) != wanted) return false;
        crc = crc_update(crc, buffer, wanted);
        total += wanted;
    }
    return (crc ^ 0xFFFFFFFFU) == expected_crc;
}
}

void tile_map_create(lv_obj_t *panel)
{
    lv_obj_update_layout(panel);
    image = lv_img_create(panel);
    lv_obj_center(image);
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
    // A 256px tile is scaled to fit the panel, preserving its aspect ratio.
    const int width = lv_obj_get_content_width(panel);
    const int height = lv_obj_get_content_height(panel);
    image_scale = static_cast<uint16_t>(min(width, height));
}

void tile_map_clear_cache()
{
    if (image) lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_img_cache_invalidate_src(&descriptor);
    descriptor.data = nullptr;
    free(atlas);
    atlas = nullptr;
    free(satellite_pixels);
    satellite_pixels = nullptr;
    viewport = MapViewport{};
    using_osm = false;
}

void tile_map_set_satellite(bool enabled)
{
    satellite_selected = enabled;
    if (image) lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
}

void tile_map_pan(int dx, int dy)
{
    if (!image || !descriptor.data || lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_t *panel = lv_obj_get_parent(image);
    const int width = lv_obj_get_content_width(panel);
    const int height = lv_obj_get_content_height(panel);
    const double extent = display_size * image_scale / 256.0;
    viewport.drag(dx, dy, width, height, extent);
    lv_obj_set_pos(image, static_cast<lv_coord_t>(lround(width / 2.0 - viewport.center_x * extent)),
                  static_cast<lv_coord_t>(lround(height / 2.0 - viewport.center_y * extent)));
}

bool tile_map_load(int zoom)
{
    if (!image || zoom < 14 || zoom > 18) return false;
    const uint32_t started = millis();
    const bool cached = satellite_selected ? satellite_pixels != nullptr : atlas != nullptr;
    static_assert(sizeof(lv_color_t) == 2, "Tile display requires LV_COLOR_DEPTH=16");
    // One detailed coverage image supports both dragging and zooming without I/O.
    if (satellite_selected && !satellite_pixels) {
        File file = map_storage().open("/satellite.t565", FILE_READ);
        if (!file || !valid_header(file, true)) {
            Serial.println("MAP ERROR missing or invalid /satellite.t565");
            return false;
        }
        auto *buffer = static_cast<lv_color_t *>(heap_caps_malloc(SAT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!buffer) return false;
        size_t read = 0;
        while (read < SAT_BYTES) {
            const size_t count = min(size_t(2048), SAT_BYTES - read);
            if (file.read(reinterpret_cast<uint8_t *>(buffer) + read, count) != count) {
                free(buffer);
                Serial.println("MAP ERROR satellite SD read failed");
                return false;
            }
            read += count;
            delay(1);
        }
#if LV_COLOR_16_SWAP
        for (size_t i = 0; i < SAT_SIZE * SAT_SIZE; ++i) {
            const uint16_t value = buffer[i].full;
            buffer[i].full = static_cast<uint16_t>((value >> 8) | (value << 8));
        }
#endif
        satellite_pixels = buffer;
    }
    if (!satellite_selected && !atlas) {
        atlas = static_cast<lv_color_t *>(heap_caps_malloc(ATLAS_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!atlas || !osm_map_render(atlas, ATLAS_SIZE, ATLAS_SIZE, 15)) {
            Serial.println("MAP CACHE ERROR preparing coverage image");
            tile_map_clear_cache();
            return false;
        }
    }
    lv_img_cache_invalidate_src(&descriptor);
    display_size = satellite_selected ? SAT_SIZE : ATLAS_SIZE;
    descriptor.header.always_zero = 0;
    descriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
    descriptor.header.w = display_size;
    descriptor.header.h = display_size;
    descriptor.data_size = satellite_selected ? SAT_BYTES : ATLAS_BYTES;
    descriptor.data = reinterpret_cast<const uint8_t *>(satellite_selected ? satellite_pixels : atlas);
    lv_img_set_src(image, &descriptor);
    lv_obj_t *panel = lv_obj_get_parent(image);
    lv_obj_update_layout(panel);
    const int width = lv_obj_get_content_width(panel);
    const int height = lv_obj_get_content_height(panel);
    image_scale = static_cast<uint16_t>(max(1, static_cast<int>(lround(
        min(width, height) * 256.0 / display_size * pow(2.0, zoom - 15)))));
    lv_obj_set_align(image, LV_ALIGN_TOP_LEFT);
    lv_img_set_pivot(image, 0, 0);
    lv_img_set_zoom(image, image_scale);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_HIDDEN);
    tile_map_pan(0, 0);
    lv_obj_update_layout(panel);
    lv_obj_invalidate(image);
    using_osm = !satellite_selected;
    Serial.printf("MAP CACHE zoom=%d hit=%d switch_ms=%lu bytes=%u\n", zoom, cached,
                  static_cast<unsigned long>(millis() - started), static_cast<unsigned>(ATLAS_BYTES));
    Serial.printf("MAP VIEW panel=%dx%d image=%dx%d scale=%u position=%d,%d hidden=%d\n",
                  width, height, lv_obj_get_width(image), lv_obj_get_height(image), image_scale,
                  lv_obj_get_x(image), lv_obj_get_y(image), lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
    Serial.printf("MAP DISPLAYED %s %dx%d draggable\n", satellite_selected ? "/satellite.t565" : "/map.osm", display_size, display_size);
    return true;
}

bool tile_map_uses_osm() { return using_osm; }

bool read_serial_line(char *line, size_t capacity, uint32_t timeout_ms)
{
    size_t used = 0;
    const uint32_t started = millis();
    while (millis() - started < timeout_ms) {
        while (Serial.available()) {
            const char c = static_cast<char>(Serial.read());
            if (c == '\r') continue;
            if (c == '\n') {
                line[used] = '\0';
                return true;
            }
            if (used + 1 >= capacity) return false;
            line[used++] = c;
        }
        delay(1);
    }
    return false;
}

int tile_map_receive(const char *arguments, bool osm_file, bool satellite_file)
{
    int zoom = 15;
    unsigned int length;
    unsigned long expected_crc;
    char extra;
    const bool valid = (osm_file || satellite_file) ?
        sscanf(arguments, "%u %lx %c", &length, &expected_crc, &extra) == 2 && length >= 5 && length <= 32 * 1024 * 1024 :
        sscanf(arguments, "%d %u %lx %c", &zoom, &length, &expected_crc, &extra) == 3 && zoom >= 14 && zoom <= 16 && length == TILE_BYTES;
    if (!valid || expected_crc > 0xFFFFFFFFUL || (satellite_file && length != SAT_BYTES + 8)) {
        Serial.println("TILE ERROR invalid zoom/size/CRC arguments");
        return -1;
    }
    auto &storage = map_storage();
    Serial.println("TILE STEP check/create directory");
    if (!osm_file && !satellite_file && !storage.exists("/tiles") && !storage.mkdir("/tiles")) {
        Serial.println("TILE ERROR cannot create tiles directory");
        return -1;
    }
    char path[40], temporary[48], backup[48];
    if (satellite_file) snprintf(path, sizeof(path), "/satellite.t565");
    else if (osm_file) snprintf(path, sizeof(path), "/map.osm");
    else snprintf(path, sizeof(path), "/tiles/thika_z%d.t565", zoom);
    snprintf(temporary, sizeof(temporary), "%s.part", path);
    snprintf(backup, sizeof(backup), "%s.bak", path);
    // Do not overwrite a backup left by an interrupted earlier replacement.
    if (storage.exists(backup)) {
        Serial.println("TILE ERROR backup exists; inspect SD before retrying");
        return -1;
    }
    if (storage.exists(temporary) && !storage.remove(temporary)) {
        Serial.println("TILE ERROR cannot remove incomplete upload");
        return -1;
    }
    Serial.println("TILE STEP create temporary file");
    File output = storage.open(temporary, FILE_WRITE);
    if (!output) {
        Serial.println("TILE ERROR cannot create temporary file");
        return -1;
    }
    Serial.println("TILE READY");
    uint8_t buffer[CHUNK_BYTES];
    size_t total = 0;
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t sequence = 0;
    uint32_t previous_crc = 0;
    size_t previous_length = 0;
    while (total < length) {
        char frame[80];
        if (!read_serial_line(frame, sizeof(frame), RECEIVE_TIMEOUT_MS)) {
            output.close();
            storage.remove(temporary);
            Serial.printf("TILE ERROR timed out waiting for frame %u\n", static_cast<unsigned int>(sequence));
            return -1;
        }
        unsigned int frame_sequence, frame_length;
        unsigned long frame_crc;
        char extra;
        const bool parsed = sscanf(frame, "TILECHUNK %u %u %lx %c", &frame_sequence, &frame_length, &frame_crc, &extra) == 3;
        const bool duplicate = parsed && sequence > 0 && frame_sequence == sequence - 1;
        const size_t wanted = duplicate ? previous_length : min(sizeof(buffer), static_cast<size_t>(length) - total);
        if (!parsed || (!duplicate && frame_sequence != sequence) || frame_length != wanted || frame_crc > 0xFFFFFFFFUL ||
            (duplicate && frame_crc != previous_crc)) {
            output.close();
            storage.remove(temporary);
            Serial.printf("TILE ERROR frame out of sync; expected sequence %u length %u\n",
                          static_cast<unsigned int>(sequence), static_cast<unsigned int>(wanted));
            return -1;
        }
        size_t received = 0;
        uint32_t deadline_start = millis();
        while (received < wanted && millis() - deadline_start < RECEIVE_TIMEOUT_MS) {
            const int available = Serial.available();
            if (available > 0) {
                const size_t count = min(static_cast<size_t>(available), wanted - received);
                const size_t got = Serial.read(buffer + received, count);
                received += got;
                if (got) deadline_start = millis();
            } else {
                delay(1);
            }
        }
        if (received != wanted) {
            output.close();
            storage.remove(temporary);
            Serial.printf("TILE ERROR frame %u received %u of %u bytes\n", static_cast<unsigned int>(sequence),
                          static_cast<unsigned int>(received), static_cast<unsigned int>(wanted));
            return -1;
        }
        if ((crc_update(0xFFFFFFFFU, buffer, wanted) ^ 0xFFFFFFFFU) != frame_crc) {
            output.close();
            storage.remove(temporary);
            Serial.printf("TILE ERROR frame %u checksum mismatch\n", static_cast<unsigned int>(sequence));
            return -1;
        }
        if (duplicate) {
            Serial.printf("TILEACK %u %u\n", frame_sequence, static_cast<unsigned int>(total));
            Serial.flush();
            continue;
        }
        const size_t written = output.write(buffer, wanted);
        if (written != wanted) {
            output.close();
            storage.remove(temporary);
            Serial.printf("TILE ERROR SD write at frame %u: %u of %u bytes\n", static_cast<unsigned int>(sequence),
                          static_cast<unsigned int>(written), static_cast<unsigned int>(wanted));
            return -1;
        }
        crc = crc_update(crc, buffer, wanted);
        total += wanted;
        Serial.printf("TILEACK %u %u\n", static_cast<unsigned int>(sequence), static_cast<unsigned int>(total));
        Serial.flush();
        previous_crc = frame_crc;
        previous_length = wanted;
        ++sequence;
    }
    output.flush();
    output.close();
    if ((crc ^ 0xFFFFFFFFU) != expected_crc || !verify_file(temporary, expected_crc, length, osm_file, satellite_file)) {
        storage.remove(temporary);
        Serial.println("TILE ERROR checksum, header, or SD readback failure");
        return -1;
    }
    const bool had_previous = storage.exists(path);
    if (had_previous && !storage.rename(path, backup)) {
        Serial.println("TILE ERROR cannot back up previous tile");
        return -1;
    }
    if (!storage.rename(temporary, path)) {
        if (had_previous) storage.rename(backup, path);
        Serial.println("TILE ERROR cannot install tile");
        return -1;
    }
    if (had_previous && !osm_file && !storage.remove(backup)) {
        Serial.println("TILE ERROR installed but backup cleanup failed");
        return -1;
    }
    Serial.printf("TILE OK %d %08lx\n", zoom, expected_crc);
    if (osm_file && had_previous) Serial.printf("OSM BACKUP %s\n", backup);
    return zoom;
}
