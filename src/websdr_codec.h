// WebSDR (PA3FWM) audio stream decoder.
//
// NOTE: The compressed audio format is defined by PA3FWM's WebSDR software.
// Using/distributing a decoder for it requires PA3FWM's permission
// (pa3fwm@websdr.org). See README.md.
#pragma once
#include <stdint.h>
#include <stddef.h>

class WebSdrCodec {
 public:
  // Called for every decoded 16-bit PCM sample (at sampleRate()).
  typedef void (*SampleSink)(int16_t s, void* ctx);

  void begin(SampleSink sink, void* ctx);
  void reset();

  // Decode one binary websocket message. The buffer must have at least
  // 4 extra readable bytes after len (they are treated as zero).
  void feed(const uint8_t* buf, size_t len);

  uint16_t sampleRate() const { return rate_; }
  int smeter() const { return smeter_; }  // raw value, dB*10 ~ smeter/10 - 127? (see webui)
  bool squelched() const { return silentFrames_ > 0; }

 private:
  void decodeBlock(const uint8_t* b, size_t& c, int h);
  void emit(int16_t s);

  SampleSink sink_ = nullptr;
  void* ctx_ = nullptr;

  int32_t coef_[20];
  int32_t hist2_[20];  // history, stored *2 (values may have .5 fractions)
  int64_t integ_ = 0;
  int shiftParam_ = 1;   // "v": 1..5
  int32_t gain_ = 0;     // set by 0x82
  uint8_t flags_ = 0;    // set by 0x83
  uint16_t rate_ = 8000;
  int smeter_ = 0;
  int silentFrames_ = 0;
};
