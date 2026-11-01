/**
 * File: printer.c
 * Description: The receiver of what the ST prints (printer.h). The command
 *              callback copies a block into a ring and returns, so chandler
 *              answers the ST at once; the main loop writes the ring to the
 *              card, one file per job.
 */
#include "printer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aconfig.h"
#include "debug.h"
#include "ff.h"
#include "md5/md5.h"
#include "pico/stdlib.h"

typedef enum {
  JOB_NONE,     // nothing received since the last job ended
  JOB_WRITING,  // the job's file is open
  JOB_FAILED,   // the card failed: the rest of the job is dropped
} JobState;

// A job's path: the folder, eight digits and the extension.
#define PRINTER_PATH_SIZE (SETTINGS_MAX_VALUE_LENGTH + 16)

static uint8_t ring[PRINTER_RING_SIZE];
static uint32_t ringHead;  // the next byte written by the callback
static uint32_t ringTail;  // the next byte the main loop writes to the card
static uint32_t lastSequence;
static bool haveSequence;
static PrinterCounters counters;

static char folder[SETTINGS_MAX_VALUE_LENGTH];
static uint32_t idleUs;
static JobState job;
static uint32_t jobNumber;
static FIL file;
static absolute_time_t lastHeard;  // the ST's last printer command, or boot
static bool stBooted;              // the job ends once the ring is written
// NEXT_JOB changed in RAM, not yet in flash. Writing flash stops the main
// loop, so the ST gets no answer meanwhile: it waits for quiet.
static bool numberUnsaved;

// The installer's last report (PRINTER_CMD_INSTALLED), for the main loop to
// log: the callback answers first and logs nothing.
static uint32_t hookBlock;
static int32_t hookVblSlot;
static bool hookReported;

#if defined(_DEBUG) && (_DEBUG != 0)
// Debug builds log each job's size and MD5 when it closes, so a job can be
// checked against a reference without taking the card out.
static MD5Context jobMd5;

static void digestStart(void) { md5Init(&jobMd5); }

static void digestAdd(uint8_t *bytes, size_t length) {
  md5Update(&jobMd5, bytes, length);
}

static void digestLog(uint32_t number) {
  md5Finalize(&jobMd5);
  char hex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(&hex[i * 2], 3, "%02x", jobMd5.digest[i]);
  }
  DPRINTF("printer: job %08lu closed, %llu bytes, MD5 %s\n",
          (unsigned long)number, (unsigned long long)jobMd5.size, hex);
}
#else
static void digestStart(void) {}
static void digestAdd(uint8_t *bytes, size_t length) {
  (void)bytes;
  (void)length;
}
static void digestLog(uint32_t number) { (void)number; }
#endif

static int settingInt(const char *key, int fallback) {
  SettingsConfigEntry *entry = settings_find_entry(aconfig_getContext(), key);
  return entry ? atoi(entry->value) : fallback;
}

void printer_init(void) {
  ringHead = 0;
  ringTail = 0;
  haveSequence = false;
  memset(&counters, 0, sizeof(counters));
  SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  snprintf(folder, sizeof(folder), "%s", entry ? entry->value : PRINTER_FOLDER);
  int seconds = settingInt(ACONFIG_PARAM_IDLE_SECONDS, 10);
  if (seconds < PRINTER_IDLE_SECONDS_MIN) {
    seconds = PRINTER_IDLE_SECONDS_MIN;
  }
  idleUs = (uint32_t)seconds * 1000000u;
  job = JOB_NONE;
  stBooted = false;
  numberUnsaved = false;
  hookReported = false;
}

static uint32_t ringUsed(void) { return ringHead - ringTail; }

void __not_in_flash_func(printer_command_cb)(TransmissionProtocol *protocol,
                                             uint16_t *payloadPtr) {
  if (protocol->command_id == PRINTER_CMD_INSTALLED) {
    hookBlock = TPROTO_GET_PAYLOAD_PARAM32(payloadPtr);
    hookVblSlot = (int32_t)TPROTO_GET_NEXT32_PAYLOAD_PARAM32(payloadPtr);
    hookReported = true;
    lastHeard = get_absolute_time();
    return;
  }
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
  lastHeard = get_absolute_time();
  lastSequence = sequence;
  haveSequence = true;
  counters.blocks++;
  counters.bytes += length;
}

static void jobPath(char *path, size_t size, uint32_t number,
                    const char *extension) {
  snprintf(path, size, "%s/%08lu%s", folder, (unsigned long)number, extension);
}

static bool onCard(uint32_t number) {
  char path[PRINTER_PATH_SIZE];
  jobPath(path, sizeof(path), number, PRINTER_JOB_EXTENSION);
  if (f_stat(path, NULL) == FR_OK) {
    return true;
  }
  jobPath(path, sizeof(path), number, PRINTER_TEMP_EXTENSION);
  return f_stat(path, NULL) == FR_OK;
}

// A job's first bytes: its file, under the temporary name, and the next
// number set (stored in flash later, by storeNumber). Failing, the job is
// dropped until it ends.
static void openJob(void) {
  job = JOB_FAILED;
  FRESULT res = f_mkdir(folder);
  if (res != FR_OK && res != FR_EXIST) {
    DPRINTF("printer: cannot make %s (%d)\n", folder, res);
    counters.writeErrors++;
    return;
  }
  int next = settingInt(ACONFIG_PARAM_NEXT_JOB, 1);
  uint32_t number = next < 1 ? 1 : (uint32_t)next;
  while (number <= PRINTER_JOB_LAST && onCard(number)) {
    number++;
  }
  if (number > PRINTER_JOB_LAST) {
    DPRINTF("printer: no job number left\n");
    counters.writeErrors++;
    return;
  }
  char path[PRINTER_PATH_SIZE];
  jobPath(path, sizeof(path), number, PRINTER_TEMP_EXTENSION);
  res = f_open(&file, path, FA_WRITE | FA_CREATE_NEW);
  if (res != FR_OK) {
    DPRINTF("printer: cannot open %s (%d)\n", path, res);
    counters.writeErrors++;
    return;
  }
  if (settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_NEXT_JOB,
                           (int)(number + 1)) == 0) {
    numberUnsaved = true;
  }
  jobNumber = number;
  job = JOB_WRITING;
  digestStart();
  DPRINTF("printer: job %08lu opened\n", (unsigned long)number);
}

// The job's end: its file under its final name.
static void closeJob(void) {
  if (job == JOB_WRITING) {
    char temp[PRINTER_PATH_SIZE];
    char final[PRINTER_PATH_SIZE];
    jobPath(temp, sizeof(temp), jobNumber, PRINTER_TEMP_EXTENSION);
    jobPath(final, sizeof(final), jobNumber, PRINTER_JOB_EXTENSION);
    FRESULT res = f_close(&file);
    if (res == FR_OK) {
      res = f_rename(temp, final);
    }
    if (res == FR_OK) {
      counters.jobs++;
      digestLog(jobNumber);
    } else {
      DPRINTF("printer: job %08lu not closed (%d)\n",
              (unsigned long)jobNumber, res);
      counters.writeErrors++;
    }
  }
  job = JOB_NONE;
}

// NEXT_JOB into flash. Until then a power cut loses it, and the jobs already
// on the card keep their numbers taken (onCard).
static void storeNumber(void) {
  absolute_time_t start = get_absolute_time();
  int res = settings_save(aconfig_getContext(), true);
  DPRINTF("printer: NEXT_JOB %sstored, %lld us\n", res < 0 ? "not " : "",
          absolute_time_diff_us(start, get_absolute_time()));
  numberUnsaved = false;
}

void printer_loop(void) {
  if (hookReported) {
    hookReported = false;
    DPRINTF("printer: the ST's hook: block $%06lx, VBL slot %ld\n",
            (unsigned long)hookBlock, (long)hookVblSlot);
  }
  uint32_t used = ringUsed();
  if (used == 0) {
    bool quiet =
        absolute_time_diff_us(lastHeard, get_absolute_time()) >= idleUs;
    if (job != JOB_NONE && (stBooted || quiet)) {
      closeJob();
    }
    stBooted = false;
    if (numberUnsaved && quiet) {
      storeNumber();
    }
    return;
  }
  if (job == JOB_NONE) {
    openJob();
  }
  // The ring's contiguous part first; the wrapped part on the next pass.
  uint32_t start = ringTail % PRINTER_RING_SIZE;
  uint32_t chunk = PRINTER_RING_SIZE - start;
  if (chunk > used) {
    chunk = used;
  }
  ringTail += chunk;
  if (job != JOB_WRITING) {
    counters.lost += chunk;
    return;
  }
  UINT written = 0;
  FRESULT res = f_write(&file, &ring[start], chunk, &written);
  counters.written += written;
  digestAdd(&ring[start], written);
  if (res == FR_OK && written == chunk && ringUsed() == 0) {
    res = f_sync(&file);
  }
  if (res != FR_OK || written != chunk) {
    DPRINTF("printer: write failed (%d), %u of %lu bytes\n", res, written,
            (unsigned long)chunk);
    counters.writeErrors++;
    counters.lost += chunk - written;
    f_close(&file);
    job = JOB_FAILED;
  }
}

void printer_stBooted(void) {
  haveSequence = false;
  stBooted = true;
  lastHeard = get_absolute_time();
}

const PrinterCounters *printer_getCounters(void) { return &counters; }
