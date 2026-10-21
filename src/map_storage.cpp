#include "map_storage.h"

#include <Arduino.h>
#include <SD_MMC.h>

namespace {
bool mounted = false;
// Verified against Schematic/UEDX24320028E-WB-A V1.1 sch.png.
constexpr int SD_CLK = 14;
constexpr int SD_CMD = 17;
constexpr int SD_D0 = 16;
constexpr int SD_D1 = 18;
constexpr int SD_D2 = 15;
constexpr int SD_D3 = 21;
}

fs::FS &map_storage()
{
    return SD_MMC;
}

bool map_storage_begin()
{
    if (mounted) return true;
    // DAT3 must be high at startup to select native SD mode, not SPI mode.
    pinMode(SD_D1, INPUT_PULLUP);
    pinMode(SD_D2, INPUT_PULLUP);
    pinMode(SD_D3, INPUT_PULLUP);
    if (!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0)) {
        Serial.println("STORAGE ERROR could not configure SD/MMC pins");
        return false;
    }
    Serial.println("STORAGE mounting native SD/MMC: 1-bit, 1 MHz, CLK14 CMD17 D0=16; formatting disabled");
    // Frequency is in kHz. Do not format if the filesystem cannot be mounted.
    mounted = SD_MMC.begin("/sd", true, false, 1000, 5);
    if (!mounted) {
        Serial.println("STORAGE ERROR native mount failed; card may need a full power cycle after SPI mode");
        return false;
    }
    Serial.printf("STORAGE mounted: card type %u, capacity %llu MB\n",
                  SD_MMC.cardType(), SD_MMC.cardSize() / (1024 * 1024));
    return true;
}

void map_storage_diagnostics()
{
    Serial.printf("SDDIAG driver=native-SDMMC mounted=%d\n", mounted);
    if (!mounted) {
        Serial.println("SDDIAG END mount failed; no write attempted");
        return;
    }
    File map = SD_MMC.open("/map.osm", FILE_READ);
    if (!map) {
        Serial.println("SDDIAG END cannot open /map.osm; no write attempted");
        return;
    }
    uint8_t header[64];
    const size_t size = map.size();
    const size_t read = map.read(header, sizeof(header));
    map.close();
    Serial.printf("SDDIAG map_size=%u header_read=%u\n", static_cast<unsigned int>(size), static_cast<unsigned int>(read));
    if (read != min(size, sizeof(header))) {
        Serial.println("SDDIAG END map read failed; no write attempted");
        return;
    }
    // Does not allocate sectors or alter the map. Root writes are tested separately.
    Serial.printf("SDDIAG tiles_directory_exists=%d\n", SD_MMC.exists("/tiles"));
    Serial.println("SDDIAG END read checks complete");
}
