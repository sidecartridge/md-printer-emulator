/**
 * File: printer.c
 * Description: The receiver of what the ST prints (printer.h). The command
 *              callback copies a block into a ring and returns, so chandler
 *              answers the ST at once; the main loop writes the ring to the
 *              card.
 */
#include "printer.h"

#include <string.h>

#include "debug.h"
#include "ff.h"

static uint8_t ring[PRINTER_RING_SIZE];
static uint32_t ringHead;  // the next byte written by the callback
static uint32_t ringTail;  // the next byte the main loop writes to the card
static uint32_t lastSequence;
static bool haveSequence;
static PrinterCounters counters;

static FIL file;
static bool fileOpen;

void printer_init(void) {
  ringHead = 0;
  ringTail = 0;
  haveSequence = false;
  memset(&counters, 0, sizeof(counters));
  fileOpen = false;
}

static uint32_t ringUsed(void) { return ringHead - ringTail; }

void __not_in_flash_func(printer_command_cb)(TransmissionProtocol *protocol,
                                             uint16_t *payloadPtr) {
  if (protocol->command_id != PRINTER_CMD_BLOCK) {
    return;
  }
  uint32_t sequence = TPROTO_GET_PAYLOAD_PARAM32(payloadPtr);
  uint32_t length = TPROTO_GET_NEXT32_PAYLOAD_PARAM32(payloadPtr);
  TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);  // d5
  TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);  // the bytes
  // The payload: the token, d3-d5 (16 bytes), then the bytes, a word each two.
  if (length > PRINTER_BLOCK_MAX ||
      protocol->payload_size < PRINTER_BLOCK_PARAMS_BYTES + length) {
    counters.malformed++;
    return;
  }
  if (haveSequence && sequence == lastSequence) {
    counters.duplicates++;
    return;
  }
  if (PRINTER_RING_SIZE - ringUsed() < length) {
    counters.overruns++;
    lastSequence = sequence;  // a retry of a dropped block is dropped too
    haveSequence = true;
    return;
  }
  // The ST sends words: the first byte is the word's high byte.
  for (uint32_t i = 0; i < length; i++) {
    uint16_t word = payloadPtr[i / 2];
    ring[(ringHead + i) % PRINTER_RING_SIZE] =
        (i & 1) ? (uint8_t)word : (uint8_t)(word >> 8);
  }
  ringHead += length;
  lastSequence = sequence;
  haveSequence = true;
  counters.blocks++;
  counters.bytes += length;
}

static bool openFile(void) {
  if (fileOpen) {
    return true;
  }
  FRESULT res = f_mkdir(PRINTER_FOLDER);
  if (res != FR_OK && res != FR_EXIST) {
    DPRINTF("printer: cannot make %s (%d)\n", PRINTER_FOLDER, res);
    return false;
  }
  res = f_open(&file, PRINTER_RECEIVED_FILE, FA_WRITE | FA_OPEN_APPEND);
  if (res != FR_OK) {
    DPRINTF("printer: cannot open %s (%d)\n", PRINTER_RECEIVED_FILE, res);
    return false;
  }
  fileOpen = true;
  return true;
}

void printer_loop(void) {
  uint32_t used = ringUsed();
  if (used == 0) {
    return;
  }
  if (!openFile()) {
    counters.writeErrors++;
    ringTail = ringHead;  // nowhere to put it: dropped, counted
    return;
  }
  // The ring's contiguous part first; the wrapped part on the next pass.
  uint32_t start = ringTail % PRINTER_RING_SIZE;
  uint32_t chunk = PRINTER_RING_SIZE - start;
  if (chunk > used) {
    chunk = used;
  }
  UINT written = 0;
  FRESULT res = f_write(&file, &ring[start], chunk, &written);
  if (res != FR_OK || written != chunk) {
    DPRINTF("printer: write failed (%d), %u of %lu bytes\n", res, written,
            (unsigned long)chunk);
    counters.writeErrors++;
  }
  ringTail += chunk;
  counters.written += written;
  if (ringUsed() == 0) {
    f_sync(&file);
  }
}

const PrinterCounters *printer_getCounters(void) { return &counters; }
