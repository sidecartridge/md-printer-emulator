/**
 * File: usbvol.h
 * Description: The folder's finished PDFs as a read-only USB drive: a
 *              computer on the Pico W's micro-USB sees the volume of
 *              pdfvol/pdfvol.h, whose PDFs are read from the card as it asks
 *              for them. TinyUSB's mass-storage device, as md-drives-emulator
 *              sets it up, without its whole-card volume.
 *
 * USB starts once the card is mounted and the cable brings power, and runs
 * from then on, printing included. The folder is listed when USB starts and
 * again each time a PDF is finished. The volume then reads as no medium for
 * USBVOL_REMOVED_MS, and once back as a medium that changed, with another
 * serial number: a computer drops what it keeps of the volume and mounts it
 * afresh. It only learns this by asking whether the medium is ready, which
 * macOS stops doing once it may lock the medium in (PREVENT ALLOW MEDIUM
 * REMOVAL), so that lock is refused; macOS and Windows then ask every
 * second, Linux desktops every two. Nothing a computer writes is taken.
 */
#ifndef USBVOL_H
#define USBVOL_H

#include <stdbool.h>
#include <stdint.h>

#define USBVOL_VBUS_POLL_MS 500    // how often the cable's power is looked at
#define USBVOL_FILES_STEP 64       // PDFs the list grows by
#define USBVOL_REMOVED_MS 5000     // a changed volume reads as no medium
#define USBVOL_SERIAL 0x4D445052u  // "MDPR", plus the volume's changes

typedef struct {
  uint32_t pdfs;       // PDFs on the volume
  uint32_t listings;   // times the folder was listed
  uint32_t polls;      // times the computer asked whether the medium is ready
  uint32_t reads;      // reads the computer asked for
  uint32_t failed;     // reads the card could not answer
  uint32_t refused;    // writes refused
  uint32_t longestUs;  // the longest read
} UsbvolCounters;

/**
 * @brief The card is mounted: USB may start.
 */
void usbvol_init(void);

/**
 * @brief One pass of the main loop: starts USB when the cable brings power,
 * then runs TinyUSB's device task.
 */
void usbvol_loop(void);

/**
 * @brief True while a computer has the volume.
 */
bool usbvol_mounted(void);

const UsbvolCounters *usbvol_getCounters(void);

#endif  // USBVOL_H
