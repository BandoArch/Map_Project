#pragma once

#include <lvgl.h>

// Call create/load while holding the LVGL mutex (or from an LVGL callback).
void tile_map_create(lv_obj_t *panel);
bool tile_map_load(int zoom);
void tile_map_clear_cache();
void tile_map_pan(int dx, int dy);
void tile_map_set_satellite(bool enabled);
bool tile_map_uses_osm();

// Blocking USB transfer, called from Arduino loop, not the LVGL task.
// Returns the received zoom level, or -1 on error. Only fixed Thika paths allowed.
int tile_map_receive(const char *arguments, bool osm_file = false, bool satellite_file = false);
