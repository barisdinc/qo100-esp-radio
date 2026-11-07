// I2S sigma-delta audio output with its own SLC DMA driver.
//
// Why not the core i2s driver: it zeroes DMA buffers on underflow (= output
// pinned at 0 V -> audible tick whenever loop() stalls > ~30 ms, e.g. during
// TLS, HTTP or flash writes) and its write functions are not IRAM-safe.
// Here the DMA buffers are refilled *inside the DMA-EOF interrupt* directly
// from the sample ring, so audio no longer depends on loop() timing at all;
// if the ring runs dry, mid-scale silence (not 0 V) is produced.
#include "audio_out.h"
#include <Arduino.h>
#include <i2s_reg.h>
#include <ets_sys.h>

namespace audio {

// 2 x 32-bit words (64 sigma-delta bits) per audio sample.
static const int WORDS_PER_SAMPLE = 2;
static const int BITS_SHIFT = 6;  // log2(64 bits per sample)

static const int NBUF = 6;           // DMA ring: 6 x 64 words = 24 ms @ 8 kHz
static const int BUF_WORDS = 64;
static const int SAMPLES_PER_BUF = BUF_WORDS / WORDS_PER_SAMPLE;

static const uint16_t RING = 4096;      // 512 ms @ 8 kHz
static const uint16_t PREBUF = 2000;    // start after 250 ms
static const uint16_t LOWWATER = 1400;  // below: repeat 1/64 samples
static const uint16_t HIGHWATER = 2600; // above: drop 1/64 samples

struct SlcItem {
  uint32_t blocksize : 12;
  uint32_t datalen : 12;
  uint32_t unused : 5;
  uint32_t sub_sof : 1;
  uint32_t eof : 1;
  volatile uint32_t owner : 1;
  uint32_t* buf_ptr;
  SlcItem* next_link_ptr;
};

static SlcItem items[NBUF];
static uint32_t bufs[NBUF][BUF_WORDS];
static SlcItem dummyItem;
static uint32_t dummyBuf[4];

static int16_t ring[RING];
static volatile uint16_t rd = 0, wr = 0;
static volatile bool playing = false;
static volatile uint32_t nUnder = 0, nOver = 0, nIrq = 0;
static volatile uint16_t gainQ8 = 200;
static uint8_t driftCnt = 0;

// sigma-delta state (ISR only)
static int32_t i1 = 0, i2 = 0, prevX = 0;

static inline uint16_t IRAM_ATTR count() { return (uint16_t)(wr - rd) & (RING - 1); }

static int32_t IRAM_ATTR nextSample() {
  uint16_t c = count();
  if (!playing) {
    if (c < PREBUF) return 0;
    playing = true;
  }
  if (c == 0) {
    playing = false;
    nUnder++;
    return 0;
  }
  int32_t x = ring[rd];
  bool advance = true;
  if (c < LOWWATER) {
    if (++driftCnt >= 64) { driftCnt = 0; advance = false; }  // repeat
  } else if (c > HIGHWATER) {
    if (++driftCnt >= 64) { driftCnt = 0; rd = (rd + 1) & (RING - 1); }  // skip
  }
  if (advance) rd = (rd + 1) & (RING - 1);
  x = (x * (int32_t)gainQ8) >> 9;  // >>8 gain, >>1 keeps the 2nd-order SD stable
  if (x > 16000) x = 16000;
  if (x < -16000) x = -16000;
  return x;
}

// fill one DMA buffer; linear interpolation between samples reduces
// zero-order-hold images
static void IRAM_ATTR fillBuf(uint32_t* buf) {
  for (int s = 0; s < SAMPLES_PER_BUF; s++) {
    int32_t x = nextSample();
    int32_t step = (x - prevX) >> BITS_SHIFT;
    int32_t v = prevX;
    for (int w = 0; w < WORDS_PER_SAMPLE; w++) {
      uint32_t word = 0;
      for (int b = 0; b < 32; b++) {
        v += step;
        bool one = i2 >= 0;
        int32_t y = one ? 32767 : -32767;
        word = (word << 1) | (one ? 1u : 0u);
        i1 += v - y;
        i2 += i1 - y;
      }
      *buf++ = word;
    }
    prevX = x;
  }
}

static void IRAM_ATTR slcIsr(void*) {
  uint32_t st = SLCIS;
  SLCIC = 0xFFFFFFFF;
  if (st & SLCIRXEOF) {
    nIrq++;
    SlcItem* done = (SlcItem*)SLCRXEDA;
    fillBuf(done->buf_ptr);
  }
}

static void setDividers(uint32_t wordRate) {
  // word rate = 160 MHz / 32 / div1 / div2
  uint32_t base = I2SBASEFREQ / 32;
  uint32_t bestErr = 0xFFFFFFFF;
  uint8_t b1 = 1, b2 = 1;
  for (uint8_t i = 1; i < 64; i++)
    for (uint8_t j = i; j < 64; j++) {
      uint32_t r = base / i / j;
      uint32_t err = r > wordRate ? r - wordRate : wordRate - r;
      if (err < bestErr) { bestErr = err; b1 = i; b2 = j; }
    }
  uint32_t c = I2SC;
  c |= I2STXR;
  I2SC = c;
  c &= ~(I2STSM | I2SRSM | (I2SBMM << I2SBM) | (I2SBDM << I2SBD) | (I2SCDM << I2SCD));
  c |= I2SRF | I2SMR | I2SRMS | I2STMS | (b1 << I2SBD) | (b2 << I2SCD);
  I2SC = c;
  c &= ~I2STXR;
  I2SC = c;
  Serial.printf("[audio] I2S word rate %lu (target %lu)\n", (unsigned long)(base / b1 / b2),
                (unsigned long)wordRate);
}

void begin(uint16_t sampleRate) {
  pinMode(3, FUNCTION_1);  // GPIO3 = I2SO_DATA; BCLK/WS are not driven out

  for (int i = 0; i < NBUF; i++) {
    for (int k = 0; k < BUF_WORDS; k++) bufs[i][k] = 0xAAAAAAAA;  // mid-scale
    items[i].unused = 0;
    items[i].owner = 1;
    items[i].eof = 1;
    items[i].sub_sof = 0;
    items[i].datalen = BUF_WORDS * 4;
    items[i].blocksize = BUF_WORDS * 4;
    items[i].buf_ptr = bufs[i];
    items[i].next_link_ptr = &items[(i + 1) % NBUF];
  }
  dummyItem = items[0];
  dummyItem.buf_ptr = dummyBuf;
  dummyItem.datalen = dummyItem.blocksize = sizeof(dummyBuf);
  dummyItem.next_link_ptr = &dummyItem;

  ETS_SLC_INTR_DISABLE();
  SLCC0 |= SLCRXLR | SLCTXLR;
  SLCC0 &= ~(SLCRXLR | SLCTXLR);
  SLCIC = 0xFFFFFFFF;
  SLCC0 &= ~(SLCMM << SLCM);
  SLCC0 |= (1 << SLCM);
  SLCRXDC |= SLCBINR | SLCBTNR;
  SLCRXDC &= ~(SLCBRXFE | SLCBRXEM | SLCBRXFM);
  // the "RX" link feeds the I2S transmitter; the TX link only needs a valid descriptor
  SLCTXL &= ~(SLCTXLAM << SLCTXLA);
  SLCRXL &= ~(SLCRXLAM << SLCRXLA);
  SLCTXL |= (uint32_t)&dummyItem << SLCTXLA;
  SLCRXL |= (uint32_t)&items[0] << SLCRXLA;
  ETS_SLC_INTR_ATTACH(slcIsr, NULL);
  SLCIE = SLCIRXEOF;
  ETS_SLC_INTR_ENABLE();
  SLCTXL |= SLCTXLS;
  SLCRXL |= SLCRXLS;

  I2S_CLK_ENABLE();
  I2SIC = 0x3F;
  I2SIE = 0;
  I2SC &= ~I2SRST;
  I2SC |= I2SRST;
  I2SC &= ~I2SRST;
  I2SFC &= ~(I2SDE | (I2STXFMM << I2STXFM) | (I2SRXFMM << I2SRXFM));
  I2SFC |= I2SDE;
  I2SCC &= ~((I2STXCMM << I2STXCM) | (I2SRXCMM << I2SRXCM));
  setSampleRate(sampleRate);
  I2SC |= I2STXS;
}

void setSampleRate(uint16_t sampleRate) {
  if (sampleRate < 4000 || sampleRate > 24000) sampleRate = 8000;
  setDividers((uint32_t)sampleRate * WORDS_PER_SAMPLE);
}

void setGain(uint16_t q8) { gainQ8 = q8; }

void push(int16_t s) {
  uint16_t c = count();
  if (c >= RING - 1) { nOver++; return; }
  ring[wr] = s;
  wr = (wr + 1) & (RING - 1);
}

void flush() {
  ETS_SLC_INTR_DISABLE();
  rd = wr;
  playing = false;
  ETS_SLC_INTR_ENABLE();
}

void pump() {}  // DMA is refilled from the interrupt now

uint16_t fill() { return count(); }
uint32_t underruns() { return nUnder; }
uint32_t overruns() { return nOver; }
uint32_t irqs() { return nIrq; }

}  // namespace audio
