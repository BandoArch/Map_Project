#include "../src/osm_xml.h"
#include <algorithm>
#include <cassert>
#include <string>
#include <cstdio>

struct Input {
    std::string text;
    size_t position = 0;
    static int read(void *context, uint8_t *buffer, size_t capacity)
    {
        auto &self = *static_cast<Input *>(context);
        const size_t size = std::min({capacity, size_t(3), self.text.size() - self.position});
        std::memcpy(buffer, self.text.data() + self.position, size);
        self.position += size;
        return static_cast<int>(size);
    }
};

int main(int argc, char **argv)
{
    Input input{"<?xml version='1.0'?><osm><node lon='37.1'\n id = \"123456789012\" lat='-1.04'/><way><nd ref='123456789012'/><tag k='highway' v='primary'/></way></osm>"};
    osm::Tags tags(Input::read, &input);
    char tag[128], value[32];
    assert(tags.next(tag, sizeof(tag)) && osm::element(tag, "osm"));
    assert(tags.next(tag, sizeof(tag)) && osm::element(tag, "node"));
    int64_t id;
    double lat;
    assert(osm::integer(tag, "id", id) && id == 123456789012LL);
    assert(osm::number(tag, "lat", lat) && std::abs(lat + 1.04) < 1e-9);
    assert(!osm::attribute(tag, "ref", value, sizeof(value)));
    assert(tags.next(tag, sizeof(tag)) && osm::element(tag, "way"));
    assert(tags.next(tag, sizeof(tag)) && osm::integer(tag, "ref", id));
    assert(tags.next(tag, sizeof(tag)) && osm::attribute(tag, "k", value, sizeof(value)) && !std::strcmp(value, "highway"));
    assert(tags.next(tag, sizeof(tag)) && osm::element(tag, "/way"));
    assert(tags.next(tag, sizeof(tag)) && osm::element(tag, "/osm"));
    assert(!tags.next(tag, sizeof(tag)) && !tags.failed());
    assert(!osm::integer("node id='999999999999999999999999'", "id", id));
    assert(!osm::number("node lat='nan'", "lat", lat));
    assert(!osm::number("node lat='1junk'", "lat", lat));
    assert(!osm::element("nodes", "node"));
    Input oversized{"<tag k='description' v='" + std::string(300, 'x') + "> quoted'/><node id='7'/>"};
    osm::Tags large(Input::read, &oversized);
    assert(large.next(tag, sizeof(tag)) && osm::element(tag, "node"));
    Input truncated{"<node id='1'"};
    osm::Tags broken(Input::read, &truncated);
    assert(!broken.next(tag, sizeof(tag)) && broken.failed());
    double x0 = -100, y0 = 128, x1 = 1000, y1 = 128;
    assert(osm::clip(x0, y0, x1, y1, 256, 256) && x0 == 0 && x1 == 255);
    x0 = 128; y0 = -100; x1 = 128; y1 = 1000;
    assert(osm::clip(x0, y0, x1, y1, 256, 256) && y0 == 0 && y1 == 255);
    x0 = -100; y0 = -100; x1 = -1; y1 = -1;
    assert(!osm::clip(x0, y0, x1, y1, 256, 256));
    if (argc == 2) {
        FILE *file = std::fopen(argv[1], "rb");
        assert(file);
        osm::Tags actual([](void *context, uint8_t *buffer, size_t size) {
            return static_cast<int>(std::fread(buffer, 1, size, static_cast<FILE *>(context)));
        }, file);
        char actual_tag[512];
        size_t nodes = 0, ways = 0;
        while (actual.next(actual_tag, sizeof(actual_tag))) {
            if (osm::element(actual_tag, "node")) {
                double lon;
                assert(osm::integer(actual_tag, "id", id) && osm::number(actual_tag, "lat", lat) && osm::number(actual_tag, "lon", lon));
                ++nodes;
            }
            if (osm::element(actual_tag, "way")) ++ways;
        }
        assert(!actual.failed() && nodes > 0 && ways > 0);
        std::fclose(file);
        std::printf("OSM parser verified: %zu nodes, %zu ways\n", nodes, ways);
    }
}
