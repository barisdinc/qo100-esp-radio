#include "ws_client.h"
#include <WiFiClientSecureBearSSL.h>
#include <base64.h>

bool WsClient::connect(const char* host, uint16_t port, bool tls,
                       const char* path, const char* origin) {
  close();
  if (tls) {
    auto* s = new BearSSL::WiFiClientSecure();
    // Public, unauthenticated audio stream: certificate is not validated.
    s->setInsecure();
    if (s->probeMaxFragmentLength(host, port, 1024)) {
      s->setBufferSizes(1024, 512);
      Serial.println(F("[ws] TLS MFLN 1024 ok"));
    } else {
      s->setBufferSizes(16384 + 325, 512);
      Serial.println(F("[ws] TLS full 16k buffer"));
    }
    cli_ = s;
  } else {
    cli_ = new WiFiClient();
  }
  cli_->setTimeout(5000);
  Serial.printf("[ws] connecting %s:%u%s heap=%u\n", host, port, tls ? " (tls)" : "",
                ESP.getFreeHeap());
  if (!cli_->connect(host, port)) {
    Serial.println(F("[ws] tcp connect failed"));
    close();
    return false;
  }
  cli_->setNoDelay(true);

  uint8_t key[16];
  for (int i = 0; i < 16; i++) key[i] = (uint8_t)RANDOM_REG32;
  String req;
  req.reserve(300);
  req += F("GET ");
  req += path;
  req += F(" HTTP/1.1\r\nHost: ");
  req += host;
  if (port != 80 && port != 443) { req += ':'; req += port; }
  req += F("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ");
  req += base64::encode(key, 16, false);
  req += F("\r\nOrigin: ");
  req += origin;
  req += F("\r\nUser-Agent: ESP8266-QO100-Radio\r\n\r\n");
  cli_->print(req);

  String status = cli_->readStringUntil('\n');
  if (status.indexOf(" 101") < 0) {
    Serial.print(F("[ws] handshake failed: "));
    Serial.println(status);
    close();
    return false;
  }
  // skip remaining headers
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    String line = cli_->readStringUntil('\n');
    if (line.length() <= 1) break;
  }
  st_ = S_HDR; hdrNeed_ = 2; hdrHave_ = 0; msgLen_ = 0; discard_ = false;
  lastRx_ = millis();
  Serial.printf("[ws] connected heap=%u\n", ESP.getFreeHeap());
  return true;
}

void WsClient::close() {
  if (cli_) {
    cli_->stop();
    delete cli_;
    cli_ = nullptr;
  }
}

bool WsClient::connected() { return cli_ && cli_->connected(); }

bool WsClient::sendFrame(uint8_t opcode, const uint8_t* data, size_t len) {
  if (!connected() || len > 1024) return false;
  uint8_t frame[1024 + 8];
  size_t n = 0;
  frame[n++] = 0x80 | opcode;
  if (len < 126) frame[n++] = 0x80 | len;
  else { frame[n++] = 0x80 | 126; frame[n++] = len >> 8; frame[n++] = len & 0xFF; }
  uint8_t mask[4];
  uint32_t r = RANDOM_REG32;
  memcpy(mask, &r, 4);
  memcpy(frame + n, mask, 4); n += 4;
  for (size_t i = 0; i < len; i++) frame[n++] = data[i] ^ mask[i & 3];
  return cli_->write(frame, n) == n;
}

bool WsClient::sendText(const String& s) {
  return sendFrame(0x1, (const uint8_t*)s.c_str(), s.length());
}

void WsClient::dispatch() {
  if (msgOpcode_ == 0x2 && handler_) {
    memset(buf_ + msgLen_, 0, 8);  // decoder may read a few bytes past the end
    handler_(buf_, msgLen_);
  }
  msgLen_ = 0;
}

void WsClient::loop(size_t maxBytes) {
  if (!cli_) return;
  size_t budget = maxBytes;
  while (budget > 0) {
    int avail = cli_->available();
    if (avail <= 0) break;
    if (st_ == S_HDR || st_ == S_EXT) {
      int c = cli_->read();
      if (c < 0) break;
      budget--;
      hdr_[hdrHave_++] = (uint8_t)c;
      if (hdrHave_ < hdrNeed_) continue;
      if (st_ == S_HDR) {
        fin_ = hdr_[0] & 0x80;
        opcode_ = hdr_[0] & 0x0F;
        uint8_t l7 = hdr_[1] & 0x7F;  // server frames are not masked
        if (l7 == 126) { st_ = S_EXT; hdrNeed_ = 4; continue; }
        if (l7 == 127) { st_ = S_EXT; hdrNeed_ = 10; continue; }
        payLen_ = l7;
      } else if (hdrNeed_ == 4) {
        payLen_ = ((size_t)hdr_[2] << 8) | hdr_[3];
      } else {
        payLen_ = ((size_t)hdr_[6] << 24) | ((size_t)hdr_[7] << 16) |
                  ((size_t)hdr_[8] << 8) | hdr_[9];
      }
      // start payload
      payHave_ = 0;
      if (opcode_ != 0x0) {  // new message (not continuation)
        if (opcode_ < 0x8) { msgOpcode_ = opcode_; msgLen_ = 0; discard_ = false; }
      }
      st_ = S_PAYLOAD;
      if (opcode_ < 0x8 && msgLen_ + payLen_ > MAXMSG) discard_ = true;
      if (payLen_ == 0) goto frame_done;
      continue;
    }
    // S_PAYLOAD
    {
      size_t want = payLen_ - payHave_;
      if (want > (size_t)avail) want = avail;
      if (want > budget) want = budget;
      if (opcode_ >= 0x8) {
        // control frame (<=125 bytes)
        if (payHave_ + want > sizeof(ctl_)) want = sizeof(ctl_) - payHave_;
        int r = want ? cli_->read(ctl_ + payHave_, want) : 0;
        if (r <= 0) break;
        payHave_ += r; budget -= r;
        if (payHave_ < payLen_) continue;
      } else if (discard_) {
        uint8_t tmp[64];
        int r = cli_->read(tmp, want > sizeof(tmp) ? sizeof(tmp) : want);
        if (r <= 0) break;
        payHave_ += r; budget -= r;
        if (payHave_ < payLen_) continue;
      } else {
        int r = cli_->read(buf_ + msgLen_, want);
        if (r <= 0) break;
        msgLen_ += r; payHave_ += r; budget -= r;
        if (payHave_ < payLen_) continue;
      }
    }
  frame_done:
    lastRx_ = millis();
    if (opcode_ == 0x9) sendFrame(0xA, ctl_, payLen_ > sizeof(ctl_) ? 0 : payLen_);
    if (opcode_ == 0x8) { Serial.println(F("[ws] close frame")); close(); return; }
    if (opcode_ < 0x8 && fin_) {
      if (!discard_) dispatch();
      msgLen_ = 0;
      discard_ = false;
    }
    st_ = S_HDR; hdrNeed_ = 2; hdrHave_ = 0;
  }
}
