/**
 * File: emul.c
 * Author: Diego Parrilla Santamaría
 * Date: February 2025, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS
 * Description: Template code for the core emulation
 */

#include "emul.h"

#include <stdint.h>

// inclusw in the C file to avoid multiple definitions
#include "aconfig.h"
#include "blink.h"
#include "chandler.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "devhooks.h"  // Debug-only SWD mailbox; include in this file only
#include "display.h"
#include "ff.h"
#include "memfunc.h"
#include "menu.h"
#include "pages.h"
#include "pico/stdlib.h"
#include "printer.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"  // Include the target firmware binary
#include "term.h"
#include "usbvol.h"

#define SLEEP_LOOP_MS 100

// How long a sentinel command the ST must act on before this side moves on
// is held. The ST reads the sentinel once per pass of its menu loop (measured
// on an ST: within 42 ms); the hold is well over that.
#define SENTINEL_HOLD_MS 500

enum {
  APP_MODE_SETUP = 255  // Setup
};

// Keep active loop or exit
static bool keepActive = true;

#if defined(_DEBUG) && (_DEBUG != 0)
// Heap held on request by `swd.py app heap_hold`, to test running out of it.
typedef struct DevhooksHeldBlock {
  struct DevhooksHeldBlock *next;
} DevhooksHeldBlock;
static DevhooksHeldBlock *devhooksHeldHeap = NULL;

// App commands sent over SWD by tools/dev/swd.py (see emul.h).
static uint32_t emul_devhooksApp(uint16_t commandId, const uint16_t *payload,
                                 uint16_t payloadSize) {
  switch (commandId) {
    case DEVHOOKS_APP_HEAP_HOLD: {
      uint32_t kb = (payloadSize >= 2u) ? payload[0] : 0u;
      if (kb == 0u) {
        while (devhooksHeldHeap != NULL) {
          DevhooksHeldBlock *next = devhooksHeldHeap->next;
          free(devhooksHeldHeap);
          devhooksHeldHeap = next;
        }
        DPRINTF("devhooks: heap hold released\n");
        return 1;
      }
      DevhooksHeldBlock *block = malloc(sizeof(DevhooksHeldBlock) + kb * 1024u);
      if (block != NULL) {
        block->next = devhooksHeldHeap;
        devhooksHeldHeap = block;
      }
      DPRINTF("devhooks: holding %lu KB more heap: %s\n", (unsigned long)kb,
              (block != NULL) ? "ok" : "refused");
      return (block != NULL) ? 1u : 0u;
    }
    default:
      return 0;
  }
}
#endif

// Keep answering the ST for ms milliseconds, so a command in flight is not
// left without its answer while a sentinel command waits to be seen.
static void emul_serviceFor(uint32_t ms) {
  absolute_time_t until = make_timeout_time_ms(ms);
  while (absolute_time_diff_us(get_absolute_time(), until) > 0) {
    chandler_loop();
  }
}

// Should we reset the device, or jump to the booster app?
// By default, we reset the device.
static bool resetDeviceAtBoot = true;

// The menu's E: the printer's hook installed, and the ST to GEM. Refused
// until the ST has said hello: the hook relies on what it publishes then (the
// machine, for a Mega STE's cache).
bool emul_startPrinter(void) {
  if (!chandler_stPresent()) return false;
  term_printString(
      "\x1B"
      "E"
      "Starting the printer...\n");
  display_refresh();
  // CMD_START in the cartridge's sentinel: the ST's menu loop sees it within
  // a vsync and jumps to the user firmware, the printer's installer.
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_START);
  return true;
}

// The menu's X: the main loop ends, and the RP jumps to Booster.
void emul_startBooster(void) {
  term_printString(
      "\x1B"
      "E"
      "Launching Booster app...\n");
  term_printString("The computer will boot shortly...\n\n");
  term_printString("If it doesn't boot, power it on and off.\n");
  display_refresh();
  resetDeviceAtBoot = false;  // Jump to the booster app
  keepActive = false;         // Exit the active loop
}

// This section contains the functions that are called from the main loop

static bool getKeepActive() { return keepActive; }

static bool getResetDevice() { return resetDeviceAtBoot; }

static void preinit() {
  // Initialize the terminal
  term_init();

  // Clear the screen
  term_clearScreen();

  // Show the title
  menu_title();
  term_printString("\n\n");

  display_refresh();
}

void failure(const char *message) {
  // Initialize the terminal
  term_init();

  // Clear the screen
  term_clearScreen();

  // Show the title
  menu_title();
  term_printString("\n\n");
  term_printString(message);

  display_refresh();
}

static void init(void) {
  // The menu's keys, and the menu with its countdown.
  menu_init();
}

void emul_start() {
  // The anatomy of an app or microfirmware is as follows:
  // - The driver code running in the remote device (the computer)
  // - the driver code running in the host device (the rp2040/rp2350)
  //
  // The driver code running in the remote device is responsible for:
  // 1. Perform the emulation of the device (ex: a ROM cartridge)
  // 2. Handle the communication with the host device
  // 3. Handle the configuration of the driver (ex: the ROM file to load)
  // 4. Handle the communication with the user (ex: the terminal)
  //
  // The driver code running in the host device is responsible for:
  // 1. Handle the communication with the remote device
  // 2. Handle the configuration of the driver (ex: the ROM file to load)
  // 3. Handle the communication with the user (ex: the terminal)
  //
  // Hence, we effectively have two drivers running in two different devices
  // with different architectures and capabilities.
  //
  // Please read the documentation to learn to use the communication protocol
  // between the two devices in the tprotocol.h file.
  //

  // 1. Check if the host device must be initialized to perform the emulation
  //    of the device, or start in setup/configuration mode
  SettingsConfigEntry *appMode =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_MODE);
  int appModeValue = APP_MODE_SETUP;  // Setup menu
  if (appMode == NULL) {
    DPRINTF(
        "APP_MODE_SETUP not found in the configuration. Using default value\n");
  } else {
    appModeValue = atoi(appMode->value);
    DPRINTF("Start emulation in mode: %i\n", appModeValue);
  }

  // 2. Initialiaze the normal operation of the app, unless the configuration
  // option says to start the config app Or a SELECT button is (or was) pressed
  // to start the configuration section of the app

  // In this example, the flow will always start the configuration app first
  // The ROM Emulator app for example will check here if the start directly
  // in emulation mode is needed or not

  // 3. If we are here, it means the app is not in emulation mode, but in
  // setup/configuration mode

  // As a rule of thumb, the remote device (the computer) driver code must
  // be copied to the RAM of the host device where the emulation will take
  // place.
  // The code is stored as an array in the target_firmware.h file
  //
  // Copy the terminal firmware to RAM
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);
#if defined(_DEBUG) && (_DEBUG != 0)
  // The ST must see exactly the generated image.
  if (memcmp((const void *)&__rom_in_ram_start__, target_firmware,
             (size_t)target_firmware_length * sizeof(uint16_t)) != 0) {
    DPRINTF("ERROR: cartridge image in RAM does not match target_firmware\n");
  } else {
    DPRINTF("Cartridge image in RAM verified (%u words)\n",
            (unsigned)target_firmware_length);
  }
  // Nothing from a previous run may survive past the end of the image.
  {
    const uint8_t *window = (const uint8_t *)&__rom_in_ram_start__;
    size_t used = (size_t)target_firmware_length * sizeof(uint16_t);
    size_t leftovers = 0;
    for (size_t i = used; i < ROM_SIZE_BYTES * ROM_BANKS; i++) {
      if (window[i] != 0) leftovers++;
    }
    DPRINTF("Cartridge window after the image: %u non-zero bytes\n",
            (unsigned)leftovers);
  }
#endif

  // Initialize the cartridge ROM4 read engine. ROM4 reads are served entirely
  // by chained DMAs feeding the PIO TX FIFO — no CPU/IRQ involvement.
  // Without this engine the cartridge image is unreadable from the m68k,
  // so a failure here is fatal: panic instead of stumbling on with a half-
  // configured PIO/DMA setup.
  if (init_romemul(false) < 0) {
    panic("init_romemul failed: PIO/DMA claim or program load returned <0");
  }

  // Bring up the ROM3 command capture (PIO + DMA ring on GPIO 26) and the
  // command handler that polls the ring, parses the protocol, and dispatches
  // each command to the registered callbacks. commemul is similarly load-
  // bearing — without it the m68k can issue commands but the RP never sees
  // them, so any non-OK return is fatal.
  if (commemul_init() < 0) {
    panic("commemul_init failed: PIO/DMA claim or program load returned <0");
  }
  chandler_init();
  chandler_addCB(term_command_cb);
  printer_init();
  chandler_addCB(printer_command_cb);
  // The ST's printer hook takes what programs print only with this set;
  // otherwise it hands every call to TOS's own routine (printer.h).
  SET_SHARED_VAR(PRINTER_SVAR_ENABLED, PRINTER_ENABLED,
                 (uintptr_t)&__rom_in_ram_start__,
                 CHANDLER_SHARED_VARIABLES_OFFSET);

  // After this point, the remote computer can execute the code

  // 4. During the setup/configuration mode, the driver code must interact
  // with the user to configure the device. To simplify the process, the
  // terminal emulator is used to interact with the user.
  // The terminal emulator is a simple text-based interface that allows the
  // user to configure the device using text commands.
  // If you want to use a custom app in the remote computer, you can do it.
  // But it's easier to debug and code in the rp2040

  // Initialize the display
  display_setupU8g2();

  // Configure the SELECT button before anything slow (the SD card), so a
  // press is seen from here on; its edge interrupt catches one made while a
  // wait cannot poll. A short press restarts the RP. A press held
  // for SELECT_LONG_RESET is a factory reset: the global settings are erased
  // and the RP restarts into Booster, which then clears every app's settings.
  select_configure();
  select_setResetCallback(reset_device);
  select_setLongResetCallback(reset_deviceAndEraseFlash);

  // 5. Init the sd card
  // Most of the apps or microfirmwares will need to read and write files
  // to the SD card. The SD card is used to store the ROM, floppies, even
  // full hard disk files, configuration files, and other data.
  // The SD card is initialized here. If the SD card is not present, the
  // app continues and reports SD status in the terminal menu.
  // Each app or microfirmware must have a folder in the SD card where the
  // files are stored. The folder name is defined in the configuration.
  // If there is no folder in the micro SD card, the app will create it.

  // Static, not on the stack: FatFs keeps a pointer to it while the card is
  // mounted, and it would take a quarter of core 0's stack.
  static FATFS fsys;
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  char *folderName = PRINTER_FOLDER;
  if (folder == NULL) {
    DPRINTF("FOLDER not found in the configuration. Using default value\n");
  } else {
    DPRINTF("FOLDER: %s\n", folder->value);
    folderName = folder->value;
  }
  int sdcardErr = sdcard_initFilesystem(&fsys, folderName);
  if (sdcardErr != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable (error %i). Continuing without SD.\n",
            sdcardErr);
  } else {
    DPRINTF("SD card found & initialized\n");
    pages_init();
    usbvol_init();
  }

  // Initialize the display again (in case the terminal emulator changed it)
  display_setupU8g2();

  preinit();

  // 7. Now complete the terminal emulator initialization
  // The terminal emulator is used to interact with the user to configure the
  // device.
  init();

  // Blink on
  blink_on();

  // 8. Start the main loop
  // The main loop is the core of the app. It is responsible for running the
  // app, handling the user input, and performing the tasks of the app.
  // The main loop runs until the user decides to exit.
  // For testing purposes, this app only shows commands to manage the settings
  // Debug builds only: serve the SWD mailbox of tools/dev/swd.py
  devhooks_setAppHandler(emul_devhooksApp);

  DPRINTF("Start the app loop here\n");
  absolute_time_t nextChipPoll = get_absolute_time();
  while (getKeepActive()) {
    devhooks_poll();
    select_poll();
    // Drain the ROM3 command ring and dispatch to the registered callbacks on
    // every pass: the ST spins on its answer, so the loop never waits.
    chandler_loop();
    printer_loop();
    pages_loop();
    usbvol_loop();
    if (chandler_consumeStBoot()) {
      // A new ST session: nothing typed before the reset carries over, and
      // the ST gets a freshly drawn menu; the printer's job has ended and
      // its blocks are numbered afresh.
      printer_stBooted();
      term_clearInputBuffer();
      menu_stBooted();
    }
    if (chandler_consumeSharedVarSet()) {
      // The ST publishes its machine and TOS just after its hello.
      menu_stPublished();
    }
    // The LED's chip every 10 ms, which leaves the loop to the command ring.
    if (absolute_time_diff_us(nextChipPoll, get_absolute_time()) >= 0) {
      blink_poll();
      nextChipPoll = make_timeout_time_ms(10);
    }

    // Run the terminal foreground (consume the published command, render
    // output, etc.).
    term_loop();
    menu_loop();
  }

  // 9. Send RESET computer command
  // Ok, so we are done with the setup but we want to reset the computer to
  // reboot in the same microfirmware app or start the booster app

  emul_serviceFor(SLEEP_LOOP_MS);
  // We must reset the computer. Hold the command long enough for the ST's
  // menu loop to see it, and keep answering: a keystroke in flight would
  // otherwise keep the ST in its send, retrying, until the hold was over.
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_RESET);
  emul_serviceFor(SENTINEL_HOLD_MS);
  if (getResetDevice()) {
    // Reset the device
    reset_device();
  } else {
    // Before jumping to the booster app, let's clean the settings
    // Set emulation mode to 255 (setup menu)
    settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE,
                         APP_MODE_SETUP);
    settings_save(aconfig_getContext(), true);

    // Jump to the booster app
    DPRINTF("Jumping to the booster app...\n");
    reset_jump_to_booster();
  }
}
