#include "settings.h"
#include <Arduino.h>
#include <LittleFS.h>

static const uint32_t MAGIC = 0x51303031;  // "Q001"
static const char* PATH = "/cfg.bin";

Settings cfg;
static uint32_t dirtyAt = 0;

static void defaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = MAGIC;
  cfg.freqHz = DEFAULT_FREQ_HZ;
  cfg.server = 0;
  cfg.volume = 70;
  cfg.mute = 0;
  cfg.srvSquelch = 0;
  cfg.sqlDb10 = -1400;
  cfg.loHz = 300;
  cfg.hiHz = 2800;
}

void settingsLoad() {
  defaults();
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  File f = LittleFS.open(PATH, "r");
  if (!f) return;
  Settings tmp;
  if (f.read((uint8_t*)&tmp, sizeof(tmp)) == sizeof(tmp) && tmp.magic == MAGIC) {
    cfg = tmp;
    cfg.ssid[32] = 0;
    cfg.pass[64] = 0;
    if (cfg.server >= NUM_SERVERS) cfg.server = 0;
  }
  f.close();
}

void settingsSave() {
  File f = LittleFS.open(PATH, "w");
  if (f) {
    f.write((const uint8_t*)&cfg, sizeof(cfg));
    f.close();
  }
  dirtyAt = 0;
}

void settingsMarkDirty() { dirtyAt = millis() | 1; }

void settingsLoop() {
  if (dirtyAt && millis() - dirtyAt > 5000) settingsSave();
}
