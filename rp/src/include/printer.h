/**
 * File: printer.h
 * Description: The printer: what the RP shares with the ST's printer hook
 *              (target/atarist/src/inc/printer_layout.s), and the receiver of
 *              the blocks it sends.
 *
 * The switch: with anything but PRINTER_ENABLED in shared variable
 * PRINTER_SVAR_ENABLED, the hook hands every printer call to the routine it
 * replaced.
 *
 * The blocks: the hook sends what programs printed as PRINTER_CMD_BLOCK
 * commands, d3 the block's sequence number, d4 its length in bytes (at most
 * PRINTER_BLOCK_MAX), d5 0, then the bytes. A retry resends the same number:
 * the receiver drops a block whose number it has just taken.
 */
#ifndef PRINTER_H
#define PRINTER_H

#include <stdbool.h>
#include <stdint.h>

#include "tprotocol.h"

#define PRINTER_SVAR_ENABLED 2         // shared variable index
#define PRINTER_ENABLED 0x50524F4Eu    // 'PRON'

// The printer's commands: 0x08xx. The high byte is an app's number, and the
// other Multi-device apps hold 0x00 (the terminal), 0x02-0x07 (floppy, RTC
// and MIDI, GEMDRIVE, ACSI, pool fix, Mac disk), 0x0D, 0x7F (the template's
// self-check), 0xFE and 0xFF (chandler).
#define PRINTER_CMD_BASE 0x0800
#define PRINTER_CMD_BLOCK 0x0801
#define PRINTER_BLOCK_MAX 1024
#define PRINTER_BLOCK_PARAMS_BYTES 16  // the token and d3-d5, before the bytes

// Between the command callback and the main loop. A block is 1 KB; the card
// takes them in the main loop, so the ring holds what arrives meanwhile.
#define PRINTER_RING_SIZE (32 * 1024)

// Where the received bytes go until jobs exist: appended, as they arrive.
#define PRINTER_FOLDER "/PRINTER"
#define PRINTER_RECEIVED_FILE PRINTER_FOLDER "/RECEIVED.PRN"

typedef struct {
  uint32_t blocks;      // blocks taken
  uint32_t bytes;       // bytes taken
  uint32_t duplicates;  // blocks dropped: the number just taken (a retry)
  uint32_t malformed;   // blocks dropped: a length past the block or the max
  uint32_t overruns;    // blocks dropped: no room in the ring
  uint32_t written;     // bytes written to the card
  uint32_t writeErrors; // failed writes to the card
} PrinterCounters;

void printer_init(void);

/**
 * @brief The command callback (chandler_addCB): takes a block into the ring
 * and nothing else, so the ST gets its answer at once.
 */
void printer_command_cb(TransmissionProtocol *protocol, uint16_t *payloadPtr);

/**
 * @brief One pass of the main loop: writes what the ring holds to the card.
 * Never waits for the ST.
 */
void printer_loop(void);

const PrinterCounters *printer_getCounters(void);

#endif  // PRINTER_H
