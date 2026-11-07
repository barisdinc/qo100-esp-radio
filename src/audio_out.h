// Audio output: 1-bit 2nd-order sigma-delta stream generated into the I2S
// DMA (data pin = GPIO3 / RX). An external RC low-pass filter turns it into
// analog audio. Because the I2S peripheral clocks the bits out by DMA, the
// output is not disturbed by WiFi interrupts (unlike timer-driven PWM).
#pragma once
#include <stdint.h>

namespace audio {
void begin(uint16_t sampleRate);
void setSampleRate(uint16_t sampleRate);
void push(int16_t s);   // decoded audio in (from codec)
void pump();            // move samples into I2S DMA; call often
void flush();           // drop buffered audio (e.g. after retune / reconnect)
void setGain(uint16_t q8);  // 256 = 1.0
uint16_t fill();
uint32_t underruns();
uint32_t overruns();
uint32_t irqs();
}  // namespace audio
