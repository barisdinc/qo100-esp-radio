// WebSDR (PA3FWM) audio stream decoder - see websdr_codec.h
//
// Message stream = sequence of records:
//   0xF0|x, y        S-meter
//   0x80 + 128 B     128 A-law (G.711) samples
//   0x81 hi lo       sample rate
//   0x82 hi lo       decoder gain
//   0x83 x           flags (bit4: alternate predictor/integrator mode)
//   0x84             128 samples of silence (e.g. squelch closed)
//   0x85 + 6 B       true frequency info
//   0x90..0xDF       start of a compressed 128-sample block (low nibble = data)
//   0x00..0x7F       compressed 128-sample block reusing previous parameters
//
// Arithmetic intentionally mirrors the reference JavaScript client
// (int32 wrap-around, truncation toward zero, values with .5 fractions);
// the history buffer is therefore kept in units of 1/2.
#include "websdr_codec.h"
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#define CODEC_FAST IRAM_ATTR
#else
#define CODEC_FAST
#endif

static inline int32_t toI32(int64_t v) { return (int32_t)(uint32_t)(uint64_t)v; }

static int16_t alaw2lin(uint8_t a) {
  a ^= 0x55;
  int t = (a & 0x0F) << 4;
  int seg = (a & 0x70) >> 4;
  if (seg == 0) t += 8;
  else if (seg == 1) t += 0x108;
  else { t += 0x108; t <<= seg - 1; }
  return (a & 0x80) ? t : -t;
}

void WebSdrCodec::begin(SampleSink sink, void* ctx) {
  sink_ = sink;
  ctx_ = ctx;
  reset();
}

void WebSdrCodec::reset() {
  memset(coef_, 0, sizeof(coef_));
  memset(hist2_, 0, sizeof(hist2_));
  integ_ = 0;
}

void WebSdrCodec::emit(int16_t s) {
  if (sink_) sink_(s, ctx_);
}

void WebSdrCodec::feed(const uint8_t* b, size_t len) {
  for (size_t c = 0; c < len; c++) {
    uint8_t x = b[c];
    if ((x & 0xF0) == 0xF0) {
      smeter_ = 256 * (x & 15) + b[c + 1];
      c++;
    } else if (x == 0x80) {
      for (int i = 0; i < 128; i++) emit(alaw2lin(b[c + 1 + i]));
      c += 128;
      reset();
      silentFrames_ = 0;
    } else if (x >= 0x90 && x <= 0xDF) {
      shiftParam_ = 14 - (x >> 4);
      decodeBlock(b, c, 4);
      silentFrames_ = 0;
    } else if ((x & 0x80) == 0) {
      decodeBlock(b, c, 1);
      silentFrames_ = 0;
    } else if (x == 0x81) {
      rate_ = 256 * b[c + 1] + b[c + 2];
      c += 2;
    } else if (x == 0x82) {
      gain_ = 256 * b[c + 1] + b[c + 2];
      c += 2;
    } else if (x == 0x83) {
      flags_ = b[c + 1];
      c++;
    } else if (x == 0x84) {
      for (int i = 0; i < 128; i++) emit(0);
      reset();
      silentFrames_ = 4;
    } else if (x == 0x85) {
      c += 6;
    }
  }
}

void CODEC_FAST WebSdrCodec::decodeBlock(const uint8_t* b, size_t& c, int h) {
  static const int16_t thr[8] = {999, 999, 8, 4, 2, 1, 99, 99};
  const int v = shiftParam_;
  const int fshift = (flags_ & 16) ? 12 : 14;
  const bool noInteg = (flags_ & 16) != 0;

  for (int e = 0; e < 128; e++) {
    uint32_t d = ((uint32_t)b[c] << 24) | ((uint32_t)b[c + 1] << 16) |
                 ((uint32_t)b[c + 2] << 8) | b[c + 3];
    d <<= h;
    int p = 0;
    int j = 15 - v;
    if (d != 0)
      while (!(d & 0x80000000u) && p < j) { d <<= 1; p++; }
    if (p < j) { j = p; p++; d <<= 1; }
    else { j = (d >> 24) & 255; p += 8; d <<= 8; }

    int n = 0;
    if (j >= thr[v]) n++;
    if (j >= thr[v - 1]) n++;
    if (n > v - 1) n = v - 1;

    int32_t s = (int32_t)((((d >> 16) & 0xFFFF) >> (17 - v)) & (0xFFFFFFFFu << n));
    s += j << (v - 1);
    if (d & (1u << (32 - v + n))) { s |= (1 << n) - 1; s = ~s; }

    for (h += p + v - n; h >= 8; h -= 8) c++;

    // predictor
    int64_t acc2 = 0;
    for (int i = 0; i < 20; i++) acc2 += (int64_t)coef_[i] * hist2_[i];
    int32_t pr = toI32(acc2 / 2);
    pr = pr >= 0 ? (pr >> 12) : (int32_t)(((int64_t)pr + 4095) >> 12);

    int64_t k2 = 2 * (int64_t)s * gain_ + gain_;  // 2*(s*gain + gain/2)
    int32_t sq = toI32(k2 / 2) >> 4;

    for (int i = 19; i >= 0; i--) {
      int32_t prod = toI32(((int64_t)hist2_[i] * sq) / 2);
      coef_[i] = (int32_t)((uint32_t)coef_[i] + (uint32_t)(-(coef_[i] >> 7) + (prod >> fshift)));
      if (i == 0) break;
      hist2_[i] = hist2_[i - 1];
    }
    hist2_[0] = toI32(2 * (int64_t)pr + k2);

    int64_t out2 = (int64_t)hist2_[0] + 2 * (int64_t)(toI32(integ_) >> 4);
    emit((int16_t)(uint16_t)(uint32_t)toI32(out2 / 2));

    if (noInteg) integ_ = 0;
    else {
      int32_t a0 = toI32(hist2_[0] / 2);
      integ_ += (int32_t)((uint32_t)a0 << 4) >> 3;
    }
  }
  if (h == 0) c--;
}
