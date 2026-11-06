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
#include <stddef.h>
#include <stdint.h>

#include "settings.h"
#include "tprotocol.h"

#define PRINTER_SVAR_ENABLED 2         // shared variable index
#define PRINTER_ENABLED 0x50524F4Eu    // 'PRON'

// The printer's commands: 0x08xx. The high byte is an app's number, and the
// other Multi-device apps hold 0x00 (the terminal), 0x02-0x07 (floppy, RTC
// and MIDI, GEMDRIVE, ACSI, pool fix, Mac disk), 0x0D, 0x7F (the template's
// self-check), 0xFE and 0xFF (chandler).
#define PRINTER_CMD_BASE 0x0800
#define PRINTER_CMD_BLOCK 0x0801
// Once per ST boot, from the hook's installer: d3 the resident hook's state
// (0: not installed), d4 the VBL queue slot of the idle flush (-1: none).
#define PRINTER_CMD_INSTALLED 0x0802
#define PRINTER_BLOCK_MAX 1024
#define PRINTER_BLOCK_PARAMS_BYTES 16  // the token and d3-d5, before the bytes

// Between the command callback and the main loop. A block is 1 KB; the card
// takes them in the main loop, so the ring holds what arrives meanwhile.
#define PRINTER_RING_SIZE (32 * 1024)

// The jobs: each is one file in the folder (the app's FOLDER setting),
// nnnnnnnn.TMP while it receives and nnnnnnnn.PRN once IDLE_SECONDS have
// passed without a block, or the ST has booted again; then pages.c renders it
// into nnnnnnnn.pdf. The number comes from NEXT_JOB, stored in flash once the
// ST has been quiet for IDLE_SECONDS; a number already on the card, as either
// file or the PDF, is skipped.
#define PRINTER_FOLDER "/PRINTER"
#define PRINTER_JOB_EXTENSION ".PRN"
#define PRINTER_TEMP_EXTENSION ".TMP"
#define PRINTER_JOB_LAST 99999999u  // eight digits
#define PRINTER_PATH_SIZE (SETTINGS_MAX_VALUE_LENGTH + 16)
#define PRINTER_IDLE_SECONDS_MIN 1

typedef struct {
  uint32_t blocks;      // blocks taken
  uint32_t bytes;       // bytes taken
  uint32_t duplicates;  // blocks dropped: the number just taken (a retry)
  uint32_t malformed;   // blocks dropped: a length past the block or the max
  uint32_t overruns;    // blocks dropped: no room in the ring
  uint32_t written;     // bytes written to the card
  uint32_t writeErrors; // the card failed: a job not opened, written or closed
  uint32_t lost;        // bytes dropped: their job's file had failed
  uint32_t jobs;        // jobs closed under their final name
} PrinterCounters;

/**
 * @brief Reads the folder and the idle timeout from the app's settings.
 */
void printer_init(void);

/**
 * @brief The jobs' folder.
 */
const char *printer_folder(void);

/**
 * @brief A job's path: the folder, its eight digits and the extension.
 */
void printer_jobPath(char *path, size_t size, uint32_t number,
                     const char *extension);

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

/**
 * @brief The ST has booted again: its block numbers start over, and the job
 * it was printing has ended.
 */
void printer_stBooted(void);

const PrinterCounters *printer_getCounters(void);

#endif  // PRINTER_H
