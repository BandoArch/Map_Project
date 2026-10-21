#include "tile_map.h"

#include <Arduino.h>
#include "map_storage.h"
#include <esp_heap_caps.h>

namespace {
constexpr size_t PIXEL_BYTES = 256 * 256 * 2;
constexpr size_t TILE_BYTES = 8 + PIXEL_BYTES;
// A numbered, checksummed stop-and-wait frame keeps host and device aligned.
constexpr size_t CHUNK_BYTES = 128;
constexpr uint32_t RECEIVE_TIMEOUT_MS = 20000;
lv_obj_t *image = nullptr;
lv_img_dsc_t descriptor = {};
lv_color_t *pixels = nullptr;
uint16_t image_scale = 256;

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

bool valid_header(File &file)
{
    const uint8_t expected[] = {'T', '5', '6', '5', 0, 1, 0, 1};
    uint8_t header[8];
    return file.size() == TILE_BYTES && file.read(header, sizeof(header)) == sizeof(header) &&
           memcmp(header, expected, sizeof(header)) == 0;
}

bool verify_file(const char *path, uint32_t expected_crc)
{
    File file = map_storage().open(path, FILE_READ);
    if (!file || !valid_header(file) || !file.seek(0)) return false;
    uint8_t buffer[CHUNK_BYTES];
    uint32_t crc = 0xFFFFFFFFU;
    size_t total = 0;
    while (total < TILE_BYTES) {
        const size_t wanted = min(sizeof(buffer), TILE_BYTES - total);
        if (file.read(buffer, wanted) != wanted) return false;
        crc = crc_update(crc, buffer, wanted);
        total += wanted;
    }
    return (crc ^ 0xFFFFFFFFU) == expected_crc;
}
}

void tile_map_create(lv_obj_t *panel)
{
    image = lv_img_create(panel);
    lv_obj_center(image);
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
    // A 256px tile is scaled to fit the panel, preserving its aspect ratio.
    const int width = lv_obj_get_width(panel) - 2;
    const int height = lv_obj_get_height(panel) - 2;
    image_scale = static_cast<uint16_t>(min(width, height));
}

bool tile_map_load(int zoom)
{
    if (!image || zoom < 14 || zoom > 16) return false;
    char path[40];
    snprintf(path, sizeof(path), "/tiles/thika_z%d.t565", zoom);
    File file = map_storage().open(path, FILE_READ);
    if (!file || !valid_header(file)) {
        Serial.printf("MAP unavailable: %s\n", path);
        return false;
    }
    auto *next = static_cast<lv_color_t *>(heap_caps_malloc(PIXEL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!next) next = static_cast<lv_color_t *>(heap_caps_malloc(PIXEL_BYTES, MALLOC_CAP_8BIT));
    if (!next) {
        Serial.println("MAP ERROR not enough memory for tile");
        return false;
    }
    static_assert(sizeof(lv_color_t) == 2, "Tile display requires LV_COLOR_DEPTH=16");
    if (file.read(reinterpret_cast<uint8_t *>(next), PIXEL_BYTES) != PIXEL_BYTES) {
        free(next);
        Serial.println("MAP ERROR incomplete tile read");
        return false;
    }
#if LV_COLOR_16_SWAP
    // On-card format is always standard little-endian RGB565. This display's
    // LVGL color storage uses swapped bytes for the SPI LCD.
    auto *bytes = reinterpret_cast<uint8_t *>(next);
    for (size_t i = 0; i < PIXEL_BYTES; i += 2) {
        const uint8_t first = bytes[i];
        bytes[i] = bytes[i + 1];
        bytes[i + 1] = first;
    }
#endif
    lv_img_cache_invalidate_src(&descriptor);
    lv_color_t *previous = pixels;
    pixels = next;
    descriptor.header.always_zero = 0;
    descriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
    descriptor.header.w = 256;
    descriptor.header.h = 256;
    descriptor.data_size = PIXEL_BYTES;
    descriptor.data = reinterpret_cast<const uint8_t *>(pixels);
    lv_img_set_src(image, &descriptor);
    lv_img_set_zoom(image, image_scale);
    lv_obj_center(image);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(image);
    free(previous);
    Serial.printf("MAP DISPLAYED %s 256x256\n", path);
    return true;
}

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

int tile_map_receive(const char *arguments)
{
    int zoom;
    unsigned int length;
    unsigned long expected_crc;
    char extra;
    if (sscanf(arguments, "%d %u %lx %c", &zoom, &length, &expected_crc, &extra) != 3 ||
        zoom < 14 || zoom > 16 || length != TILE_BYTES || expected_crc > 0xFFFFFFFFUL) {
        Serial.println("TILE ERROR invalid zoom/size/CRC arguments");
        return -1;
    }
    auto &storage = map_storage();
    Serial.println("TILE STEP check/create directory");
    if (!storage.exists("/tiles") && !storage.mkdir("/tiles")) {
        Serial.println("TILE ERROR cannot create tiles directory");
        return -1;
    }
    char path[40], temporary[48], backup[48];
    snprintf(path, sizeof(path), "/tiles/thika_z%d.t565", zoom);
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
        const size_t wanted = min(sizeof(buffer), static_cast<size_t>(length) - total);
        if (sscanf(frame, "TILECHUNK %u %u %lx %c", &frame_sequence, &frame_length, &frame_crc, &extra) != 3 ||
            frame_sequence != sequence || frame_length != wanted || frame_crc > 0xFFFFFFFFUL) {
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
        ++sequence;
    }
    output.flush();
    output.close();
    if ((crc ^ 0xFFFFFFFFU) != expected_crc || !verify_file(temporary, expected_crc)) {
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
    if (had_previous && !storage.remove(backup)) {
        Serial.println("TILE ERROR installed but backup cleanup failed");
        return -1;
    }
    Serial.printf("TILE OK %d %08lx\n", zoom, expected_crc);
    return zoom;
}
