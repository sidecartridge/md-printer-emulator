/*
 * usbvol_none.c: the PDF volume over USB (rp/src/usbvol.c) where there is no
 * USB: EmuMD emulates none, so the main loop's calls do nothing.
 */
#include "usbvol.h"

static UsbvolCounters counters;

void usbvol_init(void) {}

void usbvol_loop(void) {}

bool usbvol_mounted(void) { return false; }

const UsbvolCounters *usbvol_getCounters(void) { return &counters; }
