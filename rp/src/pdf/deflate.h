/**
 * File: deflate.h
 * Description: A zlib stream (RFC 1950 around RFC 1951's deflate), written as
 *              the data arrives, in little RAM: repeated strings are found
 *              within the last DEFLATE_WINDOW bytes and coded with deflate's
 *              fixed Huffman codes, in one block.
 */
#ifndef DEFLATE_H
#define DEFLATE_H

#include <stdint.h>

#define DEFLATE_WINDOW 1024  // how far back a repeated string is looked for
#define DEFLATE_HASH 2048    // the hash table's entries
#define DEFLATE_CHAIN 32     // earlier strings tried for each position

// Takes the stream's next byte.
typedef void (*DeflateOut)(void *ctx, uint8_t byte);

typedef struct {
  DeflateOut out;
  void *ctx;
  uint8_t data[2 * DEFLATE_WINDOW];  // the window, then the bytes to code
  int32_t length;                    // bytes in data
  int32_t pos;                       // the next byte to code
  uint16_t head[DEFLATE_HASH];       // the last position of each hash
  uint16_t prev[DEFLATE_WINDOW];     // the position before, same hash
  uint32_t adlerA;                   // the data's Adler-32
  uint32_t adlerB;
  uint32_t bits;  // bits not yet out, the first lowest
  int nbits;
} Deflate;

void deflate_begin(Deflate *def, DeflateOut out, void *ctx);
void deflate_write(Deflate *def, const uint8_t *data, uint32_t size);
// Codes what is left and ends the stream.
void deflate_end(Deflate *def);

#endif  // DEFLATE_H
