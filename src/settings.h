#pragma once
#include <stdint.h>

#define AP_SSID "qo100"
#define AP_PASS "10489777"
#define MDNS_NAME "qo100"

#define DEFAULT_FREQ_HZ 10489777000ULL

struct ServerDef {
  const char* name;
  const char* host;
  uint16_t port;
  bool tls;
};

static const ServerDef SERVERS[] = {
    {"BATC / AMSAT-UK (eshail.batc.org.uk)", "eshail.batc.org.uk", 443, true},
    {"IS0GRB (websdr.is0grb.it:8901)", "websdr.is0grb.it", 8901, false},
};
static const int NUM_SERVERS = sizeof(SERVERS) / sizeof(SERVERS[0]);

struct Settings {
  uint32_t magic;
  char ssid[33];
  char pass[65];
  uint64_t freqHz;
  uint8_t server;
  uint8_t volume;    // 0..100
  uint8_t mute;      // 0/1
  uint8_t srvSquelch;  // 0/1 : server-side squelch
  int16_t sqlDb10;   // local squelch threshold in 0.1 dB; <= -1400 means off
  int16_t loHz;      // passband edges relative to carrier (USB)
  int16_t hiHz;
};

extern Settings cfg;
void settingsLoad();
void settingsSave();          // write now
void settingsMarkDirty();     // write a few seconds later
void settingsLoop();
