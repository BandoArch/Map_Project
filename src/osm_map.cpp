#include "osm_map.h"
#include "osm_xml.h"
#include "map_storage.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <cfloat>

namespace {
constexpr size_t MAX_NODES = 250000;
constexpr size_t MAX_REFS = 8192;
struct Node { int64_t id; float lat, lon; };
struct Data {
    Node *nodes = nullptr;
    int64_t *refs = nullptr;
    size_t count = 0, capacity = 0;
    ~Data() { free(nodes); free(refs); }
    bool append(const Node &node)
    {
        if (count == capacity) {
            if (capacity == MAX_NODES) return false;
            const size_t next = std::min(MAX_NODES, capacity ? capacity * 2 : size_t(1024));
            auto *memory = static_cast<Node *>(heap_caps_realloc(nodes, next * sizeof(Node), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!memory) return false;
            nodes = memory;
            capacity = next;
        }
        nodes[count++] = node;
        return true;
    }
    const Node *find(int64_t id) const
    {
        size_t first = 0, last = count;
        while (first < last) {
            const size_t middle = first + (last - first) / 2;
            if (nodes[middle].id < id) first = middle + 1;
            else last = middle;
        }
        return first < count && nodes[first].id == id ? &nodes[first] : nullptr;
    }
};

struct Input {
    File &file;
    size_t remaining;
    size_t consumed = 0;
};

int read_file(void *context, uint8_t *buffer, size_t size)
{
    auto &input = *static_cast<Input *>(context);
    if (!input.remaining) return 0;
    const int received = input.file.read(buffer, std::min(size, input.remaining));
    if (received <= 0) {
        Serial.printf("OSM SD READ ERROR offset=%u pending=%u errno=%d\n", static_cast<unsigned>(input.consumed), static_cast<unsigned>(input.remaining), errno);
        return -1;
    }
    input.remaining -= received;
    input.consumed += received;
    return received;
}

bool line(lv_color_t *pixels, int width, int height, double x0, double y0, double x1, double y1,
          lv_color_t color, int thickness)
{
    if (!osm::clip(x0, y0, x1, y1, width, height)) return false;
    int x = lround(x0), y = lround(y0), end_x = lround(x1), end_y = lround(y1);
    const int dx = abs(end_x - x), dy = -abs(end_y - y);
    const int sx = x < end_x ? 1 : -1, sy = y < end_y ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        for (int oy = 0; oy < thickness; ++oy) for (int ox = 0; ox < thickness; ++ox) {
            if (x + ox < width && y + oy < height) pixels[(y + oy) * width + x + ox] = color;
        }
        if (x == end_x && y == end_y) break;
        const int twice = error * 2;
        if (twice >= dy) { error += dy; x += sx; }
        if (twice <= dx) { error += dx; y += sy; }
    }
    return true;
}
}

bool osm_map_render(lv_color_t *pixels, int width, int height, int zoom)
{
    File file = map_storage().open("/map.osm", FILE_READ);
    if (!file || !file.size() || file.size() > 32 * 1024 * 1024) {
        Serial.println("OSM ERROR missing/empty /map.osm or file exceeds 32 MB");
        return false;
    }
    Data data;
    Input input{file, file.size()};
    osm::Tags tags(read_file, &input);
    char tag[512];
    double min_lat = DBL_MAX, min_lon = DBL_MAX, max_lat = -DBL_MAX, max_lon = -DBL_MAX;
    double bound_min_lat = 0, bound_min_lon = 0, bound_max_lat = 0, bound_max_lon = 0;
    bool bounds = false;
    size_t scanned = 0;
    while (tags.next(tag, sizeof(tag))) {
        if ((++scanned & 511) == 0) delay(1);
        if (osm::element(tag, "bounds")) {
            bounds = osm::number(tag, "minlat", bound_min_lat) && osm::number(tag, "minlon", bound_min_lon) &&
                     osm::number(tag, "maxlat", bound_max_lat) && osm::number(tag, "maxlon", bound_max_lon) &&
                     bound_min_lat >= -90 && bound_max_lat <= 90 && bound_min_lon >= -180 && bound_max_lon <= 180 &&
                     bound_min_lat < bound_max_lat && bound_min_lon < bound_max_lon;
        }
        if (!osm::element(tag, "node")) continue;
        int64_t id;
        double lat, lon;
        if (!osm::integer(tag, "id", id) || !osm::number(tag, "lat", lat) || !osm::number(tag, "lon", lon) ||
            lat < -90 || lat > 90 || lon < -180 || lon > 180) continue;
        if (!data.append({id, static_cast<float>(lat), static_cast<float>(lon)})) {
            Serial.println("OSM ERROR node index exceeds available PSRAM or 250000 nodes");
            return false;
        }
        min_lat = std::min(min_lat, lat); max_lat = std::max(max_lat, lat);
        min_lon = std::min(min_lon, lon); max_lon = std::max(max_lon, lon);
    }
    if (tags.failed() || data.count < 2) {
        Serial.printf("OSM ERROR coordinate_nodes=%u xml_error=%d read=%u size=%u tags=%u\n", static_cast<unsigned>(data.count), tags.failed(), static_cast<unsigned>(input.consumed), static_cast<unsigned>(file.size()), static_cast<unsigned>(scanned));
        if (file.seek(file.size() > 200 ? file.size() - 200 : 0)) {
            char tail[201];
            const size_t received = file.readBytes(tail, 200);
            tail[received] = '\0';
            Serial.printf("OSM END %s\n", tail);
        }
        return false;
    }
    std::sort(data.nodes, data.nodes + data.count, [](const Node &a, const Node &b) { return a.id < b.id; });
    if (bounds) {
        min_lat = bound_min_lat; max_lat = bound_max_lat;
        min_lon = bound_min_lon; max_lon = bound_max_lon;
    }
    const double center_lat = (min_lat + max_lat) / 2, center_lon = (min_lon + max_lon) / 2;
    const double longitude_scale = std::max(0.001, cos(center_lat * 3.141592653589793 / 180));
    const double scale = std::min((width - 16) / std::max(1e-8, (max_lon - min_lon) * longitude_scale),
                                  (height - 16) / std::max(1e-8, max_lat - min_lat)) * pow(2.0, zoom - 15);
    data.refs = static_cast<int64_t *>(heap_caps_malloc(MAX_REFS * sizeof(int64_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data.refs || !file.seek(0)) return false;
    input.remaining = file.size();
    input.consumed = 0;
    std::fill(pixels, pixels + width * height, lv_color_hex(0xe5ead8));
    tags.rewind();
    bool in_way = false, overflow = false;
    size_t refs = 0, ways = 0, segments = 0, skipped = 0;
    int feature = 0;
    while (tags.next(tag, sizeof(tag))) {
        if ((++scanned & 511) == 0) delay(1);
        if (osm::element(tag, "way")) {
            in_way = true; overflow = false; refs = 0; feature = 0;
        } else if (in_way && osm::element(tag, "nd")) {
            int64_t ref;
            if (osm::integer(tag, "ref", ref)) {
                if (refs < MAX_REFS) data.refs[refs++] = ref;
                else overflow = true;
            }
        } else if (in_way && osm::element(tag, "tag")) {
            char key[48], value[96];
            if (!osm::attribute(tag, "k", key, sizeof(key)) || !osm::attribute(tag, "v", value, sizeof(value))) continue;
            if (!strcmp(key, "highway")) {
                feature = (!strcmp(value, "primary") || !strcmp(value, "secondary") || !strcmp(value, "trunk") ||
                           !strcmp(value, "motorway")) ? 4 : 3;
            } else if (!strcmp(key, "waterway") || (!strcmp(key, "natural") && !strcmp(value, "water"))) {
                if (feature < 2) feature = 2;
            } else if (!strcmp(key, "building") && strcmp(value, "no") && !feature) feature = 1;
        } else if (in_way && osm::element(tag, "/way")) {
            in_way = false;
            if (overflow) { ++skipped; continue; }
            if (!feature || refs < 2) continue;
            ++ways;
            const lv_color_t color = lv_color_hex(feature == 4 ? 0xac6c32 : feature == 3 ? 0x52675e : feature == 2 ? 0x438aa5 : 0xa5ae9c);
            for (size_t i = 1; i < refs; ++i) {
                const Node *a = data.find(data.refs[i - 1]), *b = data.find(data.refs[i]);
                if (!a || !b) continue;
                segments += line(pixels, width, height,
                    width / 2.0 + (a->lon - center_lon) * longitude_scale * scale,
                    height / 2.0 - (a->lat - center_lat) * scale,
                    width / 2.0 + (b->lon - center_lon) * longitude_scale * scale,
                    height / 2.0 - (b->lat - center_lat) * scale, color, feature == 4 ? 2 : 1);
            }
        }
    }
    Serial.printf("OSM RENDERED nodes=%u ways=%u visible_segments=%u skipped_oversize=%u zoom=%d\n",
                  static_cast<unsigned>(data.count), static_cast<unsigned>(ways), static_cast<unsigned>(segments),
                  static_cast<unsigned>(skipped), zoom);
    return !tags.failed() && segments > 0;
}
