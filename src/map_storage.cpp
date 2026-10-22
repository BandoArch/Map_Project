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

void map_storage_end()
{
    SD_MMC.end();
    mounted = false;
}

bool map_storage_check()
{
    if (!mounted) return false;
    // cardType() is cached; a real sector read detects removal without a CD pin.
    alignas(4) uint8_t sector[512];
    return SD_MMC.readRAW(sector, 0);
}

bool map_storage_begin()
{
    if (mounted) return true;
    if (!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0)) {
        Serial.println("STORAGE ERROR could not configure SD/MMC pins");
        return false;
    }
    Serial.println("STORAGE mounting native SD/MMC: 1-bit, 20 MHz, CLK14 CMD17 D0=16; formatting disabled");
    // Frequency is in kHz. Do not format if the filesystem cannot be mounted.
    for (unsigned int attempt = 1; attempt <= 3; ++attempt) {
        // Bias CMD/DAT before clocking a cold card. DAT3 selects native mode.
        pinMode(SD_CMD, INPUT_PULLUP);
        pinMode(SD_D0, INPUT_PULLUP);
        pinMode(SD_D1, INPUT_PULLUP);
        pinMode(SD_D2, INPUT_PULLUP);
        pinMode(SD_D3, INPUT_PULLUP);
        delay(250);
        Serial.printf("STORAGE native mount attempt %u/3\n", attempt);
        mounted = SD_MMC.begin("/sd", true, false, SDMMC_FREQ_DEFAULT, 5);
        if (mounted) break;
        SD_MMC.end();
    }
    if (!mounted) {
        Serial.println("STORAGE ERROR native mount failed; check driver errors above, then use SDDIAG to retry");
        return false;
    }
    Serial.printf("STORAGE mounted via native-SDMMC: card type %u, capacity %llu MB\n",
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
    Serial.printf("SDDIAG card_type=%u capacity_mb=%llu\n",
                  SD_MMC.cardType(), SD_MMC.cardSize() / (1024 * 1024));
    File map = map_storage().open("/map.osm", FILE_READ);
    if (!map) {
        Serial.println("SDDIAG END card mounted but cannot open /map.osm; no write attempted");
        return;
    }
    uint8_t header[1024];
    const size_t size = map.size();
    const size_t read = map.read(header, sizeof(header));
    map.close();
    Serial.printf("SDDIAG map_header=%.*s\n", static_cast<int>(read), reinterpret_cast<const char *>(header));
    Serial.printf("SDDIAG map_size=%u header_read=%u\n", static_cast<unsigned int>(size), static_cast<unsigned int>(read));
    if (read != min(size, sizeof(header))) {
        Serial.println("SDDIAG END map read failed; no write attempted");
        return;
    }
    // Does not allocate sectors or alter the map. Root writes are tested separately.
    Serial.printf("SDDIAG tiles_directory_exists=%d\n", map_storage().exists("/tiles"));
    Serial.println("SDDIAG END read checks complete");
}
