/**
 * File: deflate.c
 * Description: A zlib stream with deflate's fixed Huffman codes (deflate.h).
 *
 * data holds the window (the last DEFLATE_WINDOW bytes coded) and the bytes
 * still to code. A position is coded once MAX_MATCH bytes follow it, or at
 * the end; when data is full the window slides down by DEFLATE_WINDOW.
 * Positions are kept as indexes into data, and NONE once they slide out.
 */
#include "deflate.h"

#include <string.h>

#define MIN_MATCH 3
#define MAX_MATCH 258
#define NONE 0xffff
#define ADLER_MOD 65521
#define ADLER_RUN 5552  // bytes summed before b can overflow 32 bits

// RFC 1951 3.2.5: each length code's shortest length and its extra bits,
// for codes 257-285, and the same for distance codes 0-29.
static const uint16_t lengthBase[] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t lengthExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                      1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                      4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t distanceBase[] = {
    1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t distanceExtra[] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                        4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                        9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
#define LENGTH_CODES 29
#define DISTANCE_CODES 30
#define END_OF_BLOCK 256
#define FIRST_LENGTH_CODE 257

// Bits go out lowest first (RFC 1951 3.1.1).
static void put_bits(Deflate *def, uint32_t value, int count) {
  def->bits |= value << def->nbits;
  def->nbits += count;
  while (def->nbits >= 8) {
    def->out(def->ctx, (uint8_t)def->bits);
    def->bits >>= 8;
    def->nbits -= 8;
  }
}

// A Huffman code goes out from its top bit.
static void put_code(Deflate *def, uint32_t code, int count) {
  uint32_t reversed = 0;
  for (int i = 0; i < count; i++) {
    reversed = (reversed << 1) | ((code >> i) & 1);
  }
  put_bits(def, reversed, count);
}

// A literal byte, a length's code or the block's end, with the fixed codes
// (RFC 1951 3.2.6).
static void put_symbol(Deflate *def, int symbol) {
  if (symbol <= 143) {
    put_code(def, 0x30 + symbol, 8);
  } else if (symbol <= 255) {
    put_code(def, 0x190 + (symbol - 144), 9);
  } else if (symbol <= 279) {
    put_code(def, symbol - 256, 7);
  } else {
    put_code(def, 0xc0 + (symbol - 280), 8);
  }
}

static void put_match(Deflate *def, int32_t length, int32_t distance) {
  int code = LENGTH_CODES - 1;
  while (lengthBase[code] > length) code--;
  put_symbol(def, FIRST_LENGTH_CODE + code);
  put_bits(def, length - lengthBase[code], lengthExtra[code]);
  code = DISTANCE_CODES - 1;
  while (distanceBase[code] > distance) code--;
  put_code(def, code, 5);
  put_bits(def, distance - distanceBase[code], distanceExtra[code]);
}

static uint32_t hash(const uint8_t *bytes) {
  return ((bytes[0] << 10) ^ (bytes[1] << 5) ^ bytes[2]) & (DEFLATE_HASH - 1);
}

// Enters the string at pos in the table, if three bytes are there.
static void insert(Deflate *def, int32_t pos) {
  if (pos + MIN_MATCH > def->length) return;
  uint32_t h = hash(&def->data[pos]);
  def->prev[pos & (DEFLATE_WINDOW - 1)] = def->head[h];
  def->head[h] = (uint16_t)pos;
}

// The longest earlier string equal to the one at pos; its length (0 if
// shorter than MIN_MATCH) and distance.
static int32_t longest(const Deflate *def, int32_t pos, int32_t *distance) {
  int32_t limit = def->length - pos;
  if (limit > MAX_MATCH) limit = MAX_MATCH;
  if (limit < MIN_MATCH) return 0;
  int32_t best = 0;
  uint16_t candidate = def->head[hash(&def->data[pos])];
  for (int tries = DEFLATE_CHAIN;
       tries > 0 && candidate != NONE && pos - candidate < DEFLATE_WINDOW;
       tries--) {
    const uint8_t *a = &def->data[candidate];
    const uint8_t *b = &def->data[pos];
    int32_t length = 0;
    while (length < limit && a[length] == b[length]) length++;
    if (length > best) {
      best = length;
      *distance = pos - candidate;
      if (best == limit) break;
    }
    candidate = def->prev[candidate & (DEFLATE_WINDOW - 1)];
  }
  return best >= MIN_MATCH ? best : 0;
}

// Codes the positions that have MAX_MATCH bytes after them, or all of them.
static void code(Deflate *def, int all) {
  int32_t end = all ? def->length : def->length - MAX_MATCH;
  while (def->pos < end) {
    int32_t distance = 0;
    int32_t length = longest(def, def->pos, &distance);
    if (length) {
      put_match(def, length, distance);
      for (int32_t i = 0; i < length; i++) insert(def, def->pos + i);
      def->pos += length;
    } else {
      put_symbol(def, def->data[def->pos]);
      insert(def, def->pos);
      def->pos++;
    }
  }
}

static uint16_t slid(uint16_t pos) {
  return (pos == NONE || pos < DEFLATE_WINDOW) ? NONE : pos - DEFLATE_WINDOW;
}

static void slide(Deflate *def) {
  memmove(def->data, def->data + DEFLATE_WINDOW, def->length - DEFLATE_WINDOW);
  def->length -= DEFLATE_WINDOW;
  def->pos -= DEFLATE_WINDOW;
  for (int i = 0; i < DEFLATE_HASH; i++) def->head[i] = slid(def->head[i]);
  for (int i = 0; i < DEFLATE_WINDOW; i++) def->prev[i] = slid(def->prev[i]);
}

static void adler(Deflate *def, const uint8_t *data, uint32_t size) {
  while (size > 0) {
    uint32_t run = size < ADLER_RUN ? size : ADLER_RUN;
    size -= run;
    while (run--) {
      def->adlerA += *data++;
      def->adlerB += def->adlerA;
    }
    def->adlerA %= ADLER_MOD;
    def->adlerB %= ADLER_MOD;
  }
}

void deflate_begin(Deflate *def, DeflateOut out, void *ctx) {
  def->out = out;
  def->ctx = ctx;
  def->length = 0;
  def->pos = 0;
  memset(def->head, 0xff, sizeof(def->head));
  memset(def->prev, 0xff, sizeof(def->prev));
  def->adlerA = 1;
  def->adlerB = 0;
  def->bits = 0;
  def->nbits = 0;
  // RFC 1950: deflate with a window of DEFLATE_WINDOW bytes, no dictionary,
  // the check bits making the two bytes a multiple of 31.
  uint8_t cmf = 0x08;
  for (int window = DEFLATE_WINDOW; window > 256; window >>= 1) cmf += 0x10;
  out(ctx, cmf);
  out(ctx, (uint8_t)(31 - (cmf * 256) % 31));
  put_bits(def, 1, 1);  // the last block
  put_bits(def, 1, 2);  // with the fixed codes
}

void deflate_write(Deflate *def, const uint8_t *data, uint32_t size) {
  adler(def, data, size);
  while (size > 0) {
    if (def->length == (int32_t)sizeof(def->data)) {
      code(def, 0);
      slide(def);
    }
    uint32_t room = sizeof(def->data) - def->length;
    uint32_t n = size < room ? size : room;
    memcpy(def->data + def->length, data, n);
    def->length += n;
    data += n;
    size -= n;
  }
}

void deflate_end(Deflate *def) {
  code(def, 1);
  put_symbol(def, END_OF_BLOCK);
  if (def->nbits > 0) put_bits(def, 0, 8 - def->nbits);
  uint32_t check = (def->adlerB << 16) | def->adlerA;
  for (int shift = 24; shift >= 0; shift -= 8) {
    def->out(def->ctx, (uint8_t)(check >> shift));
  }
}
