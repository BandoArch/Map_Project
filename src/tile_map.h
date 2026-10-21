#pragma once

#include <lvgl.h>

// Call create/load while holding the LVGL mutex (or from an LVGL callback).
void tile_map_create(lv_obj_t *panel);
bool tile_map_load(int zoom);

// Blocking USB transfer, called from Arduino loop, not the LVGL task.
// Returns the received zoom level, or -1 on error. Only fixed Thika paths allowed.
int tile_map_receive(const char *arguments);
