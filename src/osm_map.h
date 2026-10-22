#pragma once

#include <lvgl.h>

// Render the on-card OSM XML without requiring downloaded raster tiles.
bool osm_map_render(lv_color_t *pixels, int width, int height, int zoom);
