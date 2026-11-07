// QO-100 WebSDR internet radio for ESP8266
//
//  - Connects to the saved WiFi; if that fails, opens AP "qo100" (pw 10489777)
//    with a captive WiFi-setup page at http://192.168.4.1
//  - Streams 10489.777 MHz USB from a PA3FWM WebSDR (BATC or IS0GRB)
//  - Web UI (http://qo100.local) for fine tuning, mute, volume, squelch
//  - Audio: I2S sigma-delta on GPIO3 (RX) -> RC low-pass -> amplifier
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>

#include "settings.h"
#include "audio_out.h"
#include "websdr_codec.h"
#include "ws_client.h"
#include "web_pages.h"

static ESP8266WebServer http(80);
static DNSServer dns;
static WsClient ws;
static WebSdrCodec codec;

static bool apMode = false;
static uint32_t apSince = 0;
static bool wsUp = false;
static uint32_t nextConnectAt = 0;
static uint32_t connectFails = 0;
static bool paramsDirty = false;
static bool reconnectRequested = false;
static uint16_t curRate = 8000;

// squelch gate
static bool sqlOpen = true;
static uint32_t sqlLastAbove = 0;
static int32_t gateQ15 = 32767;  // smoothed gate gain

// ------------------------------------------------------------------ helpers
static float smeterDb() { return codec.smeter() / 10.0f - 127.0f; }

static void applyVolume() {
  // perceptual curve, 100% -> ~1.2x
  uint32_t v = cfg.volume;
  audio::setGain((uint16_t)(v * v * 307 / 10000));
}

static String freqKhzStr(uint64_t hz) {
  char b[24];
  snprintf(b, sizeof(b), "%lu.%03u", (unsigned long)(hz / 1000), (unsigned)(hz % 1000));
  return String(b);
}

static void sendParams() {
  if (!wsUp) return;
  String p = F("GET /~~param?f=");
  p += freqKhzStr(cfg.freqHz);
  p += F("&band=0&lo=");
  p += String(cfg.loHz / 1000.0f, 2);
  p += F("&hi=");
  p += String(cfg.hiHz / 1000.0f, 2);
  p += F("&mode=0&name=");
  ws.sendText(p);
  ws.sendText(String(F("GET /~~param?squelch=")) + (cfg.srvSquelch ? "1" : "0"));
  Serial.printf("[sdr] tune %s kHz USB\n", freqKhzStr(cfg.freqHz).c_str());
}

// --------------------------------------------------------------- audio path
static void onSample(int16_t s, void*) {
  // local squelch on S-meter, with 2 dB hysteresis and 500 ms hang time
  bool want = true;
  if (cfg.sqlDb10 > -1400) {
    float db = smeterDb();
    float thr = cfg.sqlDb10 / 10.0f;
    uint32_t now = millis();
    if (db >= thr) sqlLastAbove = now;
    if (sqlOpen) want = (db >= thr - 2.0f) || (now - sqlLastAbove < 500);
    else want = db >= thr;
  }
  sqlOpen = want;
  int32_t target = (want && !cfg.mute) ? 32767 : 0;
  gateQ15 += (target - gateQ15) >> 7;  // ~16 ms ramp, avoids clicks
  if (target == 0 && gateQ15 < 64) gateQ15 = 0;
  audio::push((int16_t)(((int32_t)s * gateQ15) >> 15));
}

static void onWsBinary(const uint8_t* d, size_t n) {
  codec.feed(d, n);
  if (codec.sampleRate() != curRate) {
    curRate = codec.sampleRate();
    audio::setSampleRate(curRate);
    Serial.printf("[sdr] sample rate %u\n", curRate);
  }
}

// --------------------------------------------------------------- web: radio
static void sendStatus() {
  String j;
  j.reserve(512);
  j += F("{\"conn\":"); j += wsUp ? "true" : "false";
  j += F(",\"freq\":"); j += String((unsigned long)(cfg.freqHz / 1000)); j += String((unsigned)(cfg.freqHz % 1000) + 1000).substring(1);
  j += F(",\"mute\":"); j += cfg.mute;
  j += F(",\"vol\":"); j += cfg.volume;
  j += F(",\"sql\":"); j += cfg.sqlDb10;
  j += F(",\"srvsql\":"); j += cfg.srvSquelch;
  j += F(",\"open\":"); j += (sqlOpen && !codec.squelched()) ? "true" : "false";
  j += F(",\"smeter\":"); j += String(smeterDb(), 1);
  j += F(",\"server\":"); j += cfg.server;
  j += F(",\"servers\":[");
  for (int i = 0; i < NUM_SERVERS; i++) {
    if (i) j += ',';
    j += '"'; j += SERVERS[i].name; j += '"';
  }
  j += F("],\"ip\":\""); j += WiFi.localIP().toString();
  j += F("\",\"rssi\":"); j += WiFi.RSSI();
  j += F(",\"buf\":"); j += audio::fill();
  j += F(",\"under\":"); j += audio::underruns();
  j += F(",\"heap\":"); j += ESP.getFreeHeap();
  j += '}';
  http.sendHeader(F("Cache-Control"), F("no-store"));
  http.send(200, F("application/json"), j);
}

static void handleSet() {
  if (http.hasArg("freq")) {
    uint64_t f = strtoull(http.arg("freq").c_str(), nullptr, 10);
    if (f >= 10489000000ULL && f <= 10490500000ULL) {
      cfg.freqHz = f;
      paramsDirty = true;
    }
  }
  if (http.hasArg("vol")) {
    cfg.volume = constrain(http.arg("vol").toInt(), 0, 100);
    applyVolume();
  }
  if (http.hasArg("mute")) cfg.mute = http.arg("mute").toInt() ? 1 : 0;
  if (http.hasArg("sql")) cfg.sqlDb10 = constrain(http.arg("sql").toInt(), -1400, -300);
  if (http.hasArg("srvsql")) {
    cfg.srvSquelch = http.arg("srvsql").toInt() ? 1 : 0;
    paramsDirty = true;
  }
  if (http.hasArg("server")) {
    int s = http.arg("server").toInt();
    if (s >= 0 && s < NUM_SERVERS && s != cfg.server) {
      cfg.server = s;
      reconnectRequested = true;
    }
  }
  settingsMarkDirty();
  sendStatus();
}

// ---------------------------------------------------------------- web: wifi
// Scanning hops channels and drops clients connected to our AP, so in AP
// mode the list is scanned once before the AP starts and served from cache.
static String scanCache;

static void scanNow() {
  int n = WiFi.scanNetworks();
  scanCache = "";
  for (int i = 0; i < n && i < 25; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length() || scanCache.indexOf(String("data-s=\"") + ssid + "\"") >= 0) continue;
    String esc = ssid;
    esc.replace("&", "&amp;"); esc.replace("<", "&lt;"); esc.replace("\"", "&quot;");
    scanCache += F("<li data-s=\"");
    scanCache += esc; scanCache += F("\"><span>"); scanCache += esc;
    scanCache += F("</span><small>"); scanCache += WiFi.RSSI(i);
    scanCache += (WiFi.encryptionType(i) == ENC_TYPE_NONE) ? F(" dBm") : F(" dBm &#128274;");
    scanCache += F("</small></li>");
  }
  for (int i = 0; i < n; i++)
    Serial.printf("[wifi]   %-32s %4d dBm ch%d enc%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i), WiFi.encryptionType(i));
  WiFi.scanDelete();
  Serial.printf("[wifi] scan: %d networks\n", n);
}

static void handleWifiPage() {
  Serial.printf("[http] %s\n", http.uri().c_str());
  if (!apMode) scanNow();  // safe in STA mode
  http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  http.send(200, F("text/html; charset=utf-8"), "");
  http.sendContent_P(PAGE_WIFI_HEAD);
  http.sendContent(scanCache.length() ? scanCache : String(F("<li>(ağ bulunamadı, adı elle yazın)</li>")));
  http.sendContent_P(PAGE_WIFI_TAIL);
  http.sendContent("");
}

static void handleSave() {
  String ssid = http.arg("ssid"), pass = http.arg("pass");
  if (!ssid.length()) { http.send(400, "text/plain", "ssid?"); return; }
  strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
  strlcpy(cfg.pass, pass.c_str(), sizeof(cfg.pass));
  settingsSave();
  http.send_P(200, "text/html; charset=utf-8", PAGE_SAVED);
  delay(500);
  ESP.restart();
}

static void captiveRedirect() {
  Serial.printf("[http] probe %s%s -> portal\n", http.hostHeader().c_str(), http.uri().c_str());
  http.sendHeader(F("Location"), F("http://192.168.4.1/wifi"));
  http.send(302, "text/plain", "");
}

// -------------------------------------------------------------------- setup
static bool tryConnect(bool phy11g, uint32_t timeoutMs) {
  WiFi.persistent(false);
  WiFi.setAutoConnect(false);
  WiFi.mode(WIFI_OFF);
  delay(50);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);
  WiFi.setPhyMode(phy11g ? WIFI_PHY_MODE_11G : WIFI_PHY_MODE_11N);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);  // modem sleep causes audio dropouts
  WiFi.hostname(MDNS_NAME);
  WiFi.begin(cfg.ssid, cfg.pass);
  Serial.printf("[wifi] connecting to '%s' (phy %s)", cfg.ssid, phy11g ? "11g" : "11n");
  uint32_t t0 = millis();
  int last = -1;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(100);
    int stn = (int)wifi_station_get_connect_status();
    if (stn != last) { Serial.printf("\n[wifi] t=%lums station status %d", millis() - t0, stn); last = stn; }
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

static bool connectSta() {
  if (!cfg.ssid[0]) return false;
  static WiFiEventHandler hd = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& e) {
    Serial.printf("\n[wifi] disconnect reason %d\n", (int)e.reason);
  });
  size_t pl = strlen(cfg.pass);
  Serial.printf("[wifi] password length %u%s\n", (unsigned)pl,
                (pl && (cfg.pass[0] == ' ' || cfg.pass[pl - 1] == ' ')) ? " (leading/trailing SPACE!)" : "");
  // 11g first: some 11n/ax routers never finish the handshake with ESP8266 in 11n
  if (tryConnect(true, 15000) || tryConnect(false, 15000)) return true;
  Serial.printf("[wifi] STA failed: status=%d\n", (int)WiFi.status());
  return false;
}

static void startAp() {
  apMode = true;
  apSince = millis();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  scanNow();
  WiFi.mode(WIFI_AP);
  // softAP() itself configures 192.168.4.1 + DHCP server; calling
  // softAPConfig() before it can leave DHCP not running on core 3.x.
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, 6);
  Serial.printf("[wifi] softAP %s ip=%s\n", ok ? "ok" : "FAILED", WiFi.softAPIP().toString().c_str());
  static WiFiEventHandler h1 = WiFi.onSoftAPModeStationConnected([](const WiFiEventSoftAPModeStationConnected& e) {
    Serial.printf("[wifi] station joined %02x:%02x:%02x:%02x:%02x:%02x\n", e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5]);
  });
  static WiFiEventHandler h2 = WiFi.onSoftAPModeStationDisconnected([](const WiFiEventSoftAPModeStationDisconnected&) {
    Serial.println(F("[wifi] station left"));
  });
  dns.start(53, "*", WiFi.softAPIP());
  http.on("/", handleWifiPage);
  http.on("/wifi", handleWifiPage);
  http.on("/save", HTTP_POST, handleSave);
  http.onNotFound(captiveRedirect);  // Android/iOS captive-portal probes
  http.begin();
  Serial.printf("[wifi] AP mode: SSID '%s' pass '%s' -> http://192.168.4.1\n", AP_SSID, AP_PASS);
}

static void startRadioUi() {
  http.on("/", []() { http.send_P(200, "text/html; charset=utf-8", PAGE_RADIO); });
  http.on("/api/status", sendStatus);
  http.on("/api/set", handleSet);
  http.on("/wifi", handleWifiPage);
  http.on("/save", HTTP_POST, handleSave);
  http.onNotFound([]() { http.send(404, "text/plain", "not found"); });
  http.begin();
  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", 80);
}

void setup() {
  // RX pin (GPIO3) is used as I2S audio output -> UART in TX-only mode
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  delay(100);
  Serial.println(F("\n\nQO-100 WebSDR radio"));
  settingsLoad();

  codec.begin(onSample, nullptr);
  ws.onBinary(onWsBinary);

  bool sta = connectSta();
  audio::begin(8000);
  applyVolume();
  if (sta) {
    Serial.printf("[wifi] connected, IP %s  -> http://%s.local\n",
                  WiFi.localIP().toString().c_str(), MDNS_NAME);
    startRadioUi();
  } else {
    startAp();
  }
}

// --------------------------------------------------------------------- loop
static void radioLoop() {
  uint32_t now = millis();

  if (reconnectRequested) {
    reconnectRequested = false;
    ws.close();
    wsUp = false;
    nextConnectAt = 0;
    connectFails = 0;
  }

  if (wsUp && !ws.connected()) {
    Serial.println(F("[ws] disconnected"));
    ws.close();
    wsUp = false;
    nextConnectAt = now + 3000;
  }
  if (wsUp && now - ws.lastRxMs() > 15000) {
    Serial.println(F("[ws] no data for 15 s, reconnecting"));
    ws.close();
    wsUp = false;
    nextConnectAt = now + 1000;
  }

  if (!wsUp && WiFi.status() == WL_CONNECTED && (int32_t)(now - nextConnectAt) >= 0) {
    const ServerDef& s = SERVERS[cfg.server];
    String origin = String(s.tls ? "https://" : "http://") + s.host;
    if (!s.tls && s.port != 80) { origin += ':'; origin += s.port; }
    audio::flush();
    codec.reset();
    if (ws.connect(s.host, s.port, s.tls, "/~~stream?v=11", origin.c_str())) {
      wsUp = true;
      connectFails = 0;
      sendParams();
      paramsDirty = false;
    } else {
      connectFails++;
      uint32_t backoff = min<uint32_t>(60000, 2000 * connectFails);
      nextConnectAt = millis() + backoff;
    }
  }

  if (wsUp) {
    ws.loop(1024);
    if (paramsDirty) {
      paramsDirty = false;
      sendParams();
    }
  }
}

void loop() {
  audio::pump();
  http.handleClient();
  audio::pump();

  if (apMode) {
    dns.processNextRequest();
    // nobody configured us for 5 minutes -> reboot and retry saved WiFi
    if (cfg.ssid[0] && WiFi.softAPgetStationNum() == 0 && millis() - apSince > 300000) {
      Serial.println(F("[wifi] AP idle, rebooting to retry STA"));
      ESP.restart();
    }
    if (WiFi.softAPgetStationNum() > 0) apSince = millis();
    return;
  }

  MDNS.update();
  radioLoop();
  audio::pump();
  settingsLoop();

  static uint32_t lastLog = 0;
  if (millis() - lastLog > 10000) {
    lastLog = millis();
    Serial.printf("[stat] ws=%d S=%.1fdB buf=%u under=%u over=%u irq=%u heap=%u rssi=%d\n", wsUp,
                  smeterDb(), audio::fill(), audio::underruns(), audio::overruns(), audio::irqs(),
                  ESP.getFreeHeap(), WiFi.RSSI());
  }
}
