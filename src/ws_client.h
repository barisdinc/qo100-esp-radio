// Minimal non-blocking WebSocket client (ws:// and wss://) for ESP8266.
#pragma once
#include <Arduino.h>
#include <WiFiClient.h>

class WsClient {
 public:
  typedef void (*BinaryHandler)(const uint8_t* data, size_t len);

  bool connect(const char* host, uint16_t port, bool tls, const char* path,
               const char* origin);
  void close();
  bool connected();
  bool sendText(const String& s);
  // Reads at most maxBytes from the socket; dispatches complete binary messages.
  void loop(size_t maxBytes = 1024);
  void onBinary(BinaryHandler h) { handler_ = h; }
  uint32_t lastRxMs() const { return lastRx_; }

 private:
  bool sendFrame(uint8_t opcode, const uint8_t* data, size_t len);
  void dispatch();

  WiFiClient* cli_ = nullptr;
  BinaryHandler handler_ = nullptr;

  enum { S_HDR, S_EXT, S_PAYLOAD } st_ = S_HDR;
  uint8_t hdr_[10];
  uint8_t hdrNeed_ = 2, hdrHave_ = 0;
  uint8_t opcode_ = 0, msgOpcode_ = 0;
  bool fin_ = false;
  size_t payLen_ = 0, payHave_ = 0;
  bool discard_ = false;
  uint32_t lastRx_ = 0;

  static const size_t MAXMSG = 4096;
  uint8_t buf_[MAXMSG + 8];
  uint8_t ctl_[125];
  size_t msgLen_ = 0;
};
