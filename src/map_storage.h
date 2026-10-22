#pragma once

#include <FS.h>

// All map/test file operations use the same mounted filesystem.
fs::FS &map_storage();
bool map_storage_begin();
bool map_storage_check();
void map_storage_end();
void map_storage_diagnostics();
