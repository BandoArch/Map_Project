#pragma once

#include <FS.h>

// All map/test file operations use the same native SD/MMC filesystem.
fs::FS &map_storage();
bool map_storage_begin();
void map_storage_diagnostics();
