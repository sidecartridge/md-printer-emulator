/**
 * File: menu.c
 * Description: The setup menu (menu.h), in md-devops' look: the terminal's
 *              text in fixed rows, with u8g2 drawing the dividers between the
 *              sections, the sections' icons and the bottom strip over it.
 *
 * The live lines (the job's state, the last PDF, the next job, the card's
 * free space, the USB drive, the Atari) are redrawn in place when they
 * change, never the whole screen.
 */
#include "menu.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "aconfig.h"
#include "chandler.h"
#include "debug.h"
#include "display.h"
#include "display_term.h"
#include "emul.h"
#include "ff.h"
#include "memfunc.h"
#include "pages.h"
#include "pico/stdlib.h"
#include "printer.h"
#include "sdcard.h"
#include "term.h"
#include "usbvol.h"

// The screen's rows. The rows between sections are left blank: the
// dividers are drawn in them.
#define ROW_PRINTER 1
#define ROW_PRINTER_KIND 2
#define ROW_PAPER 3
#define ROW_TIMEOUT 4
#define ROW_FOLDER 5
#define ROW_JOBS 7
#define ROW_STATE 8
#define ROW_LAST 9
#define ROW_NEXT 10
#define ROW_SD 12
#define ROW_SD_STATE 13
#define ROW_FREE 14
#define ROW_USB 16
#define ROW_USB_STATE 17
#define ROW_ATARI 19
#define ROW_MACHINE 20
#define ROW_COMMANDS 22
#define ROW_PROMPT 23
#define PROMPT "Select an option: "

#define LABEL_WIDTH 13                       // "  [P]rinter    : "
#define LINE_WIDTH (TERM_SCREEN_SIZE_X - 1)  // the last column wraps
#define VALUE_WIDTH (LINE_WIDTH - 2 - LABEL_WIDTH - 2)

// The icons, from u8g2's open iconic embedded font, at the right of their
// section's header row.
#define ICON_X (DISPLAY_WIDTH - 12)
#define ICON_COG 0x42
#define ICON_HARD_DRIVE 0x4C
#define ICON_LIGHTBULB 0x4D
#define STRIP_Y (DISPLAY_HEIGHT - DISPLAY_TERM_CHAR_HEIGHT)

// The live lines as last drawn, so each is redrawn only when it changes.
enum {
  LIVE_STATE,
  LIVE_LAST,
  LIVE_NEXT,
  LIVE_SD,
  LIVE_FREE,
  LIVE_USB,
  LIVE_MACHINE,
  LIVE_LINES
};
static char shown[LIVE_LINES][TERM_SCREEN_SIZE_X];
static const uint8_t liveRow[LIVE_LINES] = {
    ROW_STATE, ROW_LAST,      ROW_NEXT,   ROW_SD_STATE,
    ROW_FREE,  ROW_USB_STATE, ROW_MACHINE};

static bool active;  // the menu is on the screen
static int countdown;
static bool halted;
static bool restartOnHello;  // the countdown ran out with no ST to start
static absolute_time_t nextTick;
static absolute_time_t nextRefresh;
static const char *notice;  // on the strip once the countdown is stopped

static void cmdPrinter(const char *arg);
static void cmdPaper(const char *arg);
static void cmdTimeout(const char *arg);
static void cmdFolder(const char *arg);
static void cmdExit(const char *arg);
static void cmdBooster(const char *arg);

static const Command commands[] = {
    {"p", cmdPrinter}, {"a", cmdPaper}, {"t", cmdTimeout},
    {"o", cmdFolder},  {"e", cmdExit},  {"x", cmdBooster},
};

// ---- Settings -----------------------------------------------------------

static const char *setting(const char *key, const char *fallback) {
  SettingsConfigEntry *entry = settings_find_entry(aconfig_getContext(), key);
  return entry != NULL && entry->value[0] != '\0' ? entry->value : fallback;
}

static bool laserjet(void) {
  return strcasecmp(setting(ACONFIG_PARAM_PRINTER, "FX80"), "LASERJET") == 0;
}

static bool letter(void) {
  return strcasecmp(setting(ACONFIG_PARAM_PAPER, "A4"), "LETTER") == 0;
}

// Stores the settings; false, said on the strip, if flash refused them.
static bool save(void) {
  if (settings_save(aconfig_getContext(), true) == 0) return true;
  notice = "Saving the settings failed: not stored.";
  return false;
}

// ---- Drawing ------------------------------------------------------------

static void cursor(uint8_t row, uint8_t col) {
  char seq[5] = {'\x1B', 'Y', (char)(TERM_POS_Y + row),
                 (char)(TERM_POS_X + col), '\0'};
  term_printString(seq);
}

// A whole row: its old text cleared, the new cut to the line.
static void row_text(uint8_t row, const char *text) {
  char line[LINE_WIDTH + 1];
  snprintf(line, sizeof(line), "%s", text);
  cursor(row, 0);
  term_printString(
      "\x1B"
      "K");
  term_printString(line);
}

static void row_item(uint8_t row, const char *label, const char *value) {
  char line[LINE_WIDTH + 1];
  snprintf(line, sizeof(line), "  %-*s: %s", LABEL_WIDTH, label, value);
  row_text(row, line);
}

// The end of a long value, after "..", as md-devops shows a folder.
static void tail(char *out, size_t size, const char *value) {
  size_t length = strlen(value);
  if (length < size) {
    snprintf(out, size, "%s", value);
  } else {
    snprintf(out, size, "..%s", value + length - (size - 3));
  }
}

static void draw_icon(uint8_t row, uint8_t glyph, bool visible) {
  u8g2_t *u8g2 = display_getU8g2Ref();
  uint16_t top = row * DISPLAY_TERM_CHAR_HEIGHT;
  u8g2_SetDrawColor(u8g2, 0);
  u8g2_DrawBox(u8g2, ICON_X, top > 0 ? top - 1 : 0, 8, 9);
  if (visible) {
    u8g2_SetFont(u8g2, u8g2_font_open_iconic_embedded_1x_t);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_DrawGlyph(u8g2, ICON_X, top + 7, glyph);
    u8g2_SetFont(u8g2, u8g2_font_amstrad_cpc_extended_8f);
  }
  u8g2_SetDrawColor(u8g2, 1);
}

// A thin line in the blank row above a section.
static void draw_divider(uint8_t sectionRow) {
  u8g2_t *u8g2 = display_getU8g2Ref();
  u8g2_SetDrawColor(u8g2, 1);
  u8g2_DrawHLine(u8g2, 0, sectionRow * DISPLAY_TERM_CHAR_HEIGHT - 4,
                 DISPLAY_WIDTH);
}

// The bottom strip: a message in the small font, dark on light.
static void draw_strip(const char *message) {
  u8g2_t *u8g2 = display_getU8g2Ref();
  u8g2_SetDrawColor(u8g2, 1);
  u8g2_DrawBox(u8g2, 0, STRIP_Y, DISPLAY_WIDTH, DISPLAY_TERM_CHAR_HEIGHT);
  u8g2_SetFont(u8g2, u8g2_font_squeezed_b7_tr);
  u8g2_SetDrawColor(u8g2, 0);
  u8g2_DrawStr(u8g2, 2, DISPLAY_HEIGHT - 1, message != NULL ? message : "");
  u8g2_SetDrawColor(u8g2, 1);
  u8g2_SetFont(u8g2, u8g2_font_amstrad_cpc_extended_8f);
}

// The countdown as md-devops draws it: a light bar that shrinks as the
// seconds go, its text dark on the bar and light past it (two passes, each
// clipped to its half).
static void draw_countdown(int left) {
  u8g2_t *u8g2 = display_getU8g2Ref();
  uint16_t h = DISPLAY_TERM_CHAR_HEIGHT;
  uint16_t bar = (uint16_t)(DISPLAY_WIDTH * left / MENU_COUNTDOWN_SECONDS);
  char message[48];
  snprintf(message, sizeof(message),
           "Starting the printer in %d s - any key stops it", left);
  u8g2_SetDrawColor(u8g2, 0);
  u8g2_DrawBox(u8g2, 0, STRIP_Y, DISPLAY_WIDTH, h);
  u8g2_SetFont(u8g2, u8g2_font_squeezed_b7_tr);
  if (bar > 0) {
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_DrawBox(u8g2, 0, STRIP_Y, bar, h);
    u8g2_SetClipWindow(u8g2, 0, STRIP_Y, bar, STRIP_Y + h);
    u8g2_SetDrawColor(u8g2, 0);
    u8g2_DrawStr(u8g2, 2, DISPLAY_HEIGHT - 1, message);
  }
  if (bar < DISPLAY_WIDTH) {
    u8g2_SetClipWindow(u8g2, bar, STRIP_Y, DISPLAY_WIDTH, STRIP_Y + h);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_DrawStr(u8g2, 2, DISPLAY_HEIGHT - 1, message);
  }
  u8g2_SetMaxClipWindow(u8g2);
  u8g2_SetDrawColor(u8g2, 1);
  u8g2_SetFont(u8g2, u8g2_font_amstrad_cpc_extended_8f);
}

static void draw_bottom(void) {
  static char lost[48];
  bool full = false;
  uint32_t number = printer_lost(&full);
  if (halted && notice == NULL && number != 0) {
    snprintf(lost, sizeof(lost), "Card %s: job %08lu lost.",
             full ? "full" : "failed", (unsigned long)number);
    draw_strip(lost);
  } else if (halted) {
    draw_strip(notice != NULL ? notice
                              : "Countdown stopped: [E] starts the printer.");
  } else {
    draw_countdown(countdown);
  }
}

// The cursor back on the prompt, where a key is typed.
static void to_prompt(void) { cursor(ROW_PROMPT, (uint8_t)strlen(PROMPT)); }

// ---- The lines ------------------------------------------------------------

static void machine(char *out, size_t size) {
  if (!chandler_stPresent()) {
    snprintf(out, size, "no hello (reset the ST)");
    return;
  }
  uint32_t mch = 0;
  uint32_t versions = 0;
  GET_SHARED_VAR(CHANDLER_HARDWARE_TYPE, &mch, (uintptr_t)&__rom_in_ram_start__,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
  GET_SHARED_VAR(CHANDLER_SVERSION, &versions, (uintptr_t)&__rom_in_ram_start__,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
  const char *name = mch == 0x00000000   ? "ST"
                     : mch == 0x00010000 ? "STE"
                     : mch == 0x00010001 ? "ST Book"
                     : mch == 0x00010010 ? "Mega STE"
                     : mch == 0x00020000 ? "TT"
                     : mch == 0x00030000 ? "Falcon"
                                         : NULL;
  uint32_t tos = versions >> 16;
  if (name != NULL) {
    snprintf(out, size, "%s, TOS %lx.%02lx", name, (unsigned long)(tos >> 8),
             (unsigned long)(tos & 0xFF));
  } else {
    snprintf(out, size, "_MCH %08lx, TOS %lx.%02lx", (unsigned long)mch,
             (unsigned long)(tos >> 8), (unsigned long)(tos & 0xFF));
  }
}

// The live line's text, as it is now.
static void live_text(int which, char *out, size_t size) {
  char value[TERM_SCREEN_SIZE_X];
  const PagesCounters *pages = pages_getCounters();
  switch (which) {
    case LIVE_STATE: {
      bool full = false;
      uint32_t receiving = printer_receiving();
      uint32_t rendering = pages_rendering();
      if (receiving != 0) {
        snprintf(value, sizeof(value), "receiving %08lu",
                 (unsigned long)receiving);
      } else if (rendering != 0) {
        snprintf(value, sizeof(value), "making %08lu.pdf",
                 (unsigned long)rendering);
      } else if (printer_lost(&full) != 0) {
        snprintf(value, sizeof(value), "%08lu lost: card %s",
                 (unsigned long)printer_lost(NULL), full ? "full" : "failed");
      } else {
        snprintf(value, sizeof(value), "idle");
      }
      snprintf(out, size, "  %-*s: %s", LABEL_WIDTH, "State", value);
      return;
    }
    case LIVE_LAST:
      if (pages->lastJob == 0) {
        snprintf(value, sizeof(value), "none yet");
      } else if (pages->lastPages == 0) {
        snprintf(value, sizeof(value), "%08lu: no page",
                 (unsigned long)pages->lastJob);
      } else {
        snprintf(value, sizeof(value), "%08lu.pdf, %lu page%s",
                 (unsigned long)pages->lastJob, (unsigned long)pages->lastPages,
                 pages->lastPages == 1 ? "" : "s");
      }
      snprintf(out, size, "  %-*s: %s", LABEL_WIDTH, "Last PDF", value);
      return;
    case LIVE_NEXT:
      snprintf(value, sizeof(value), "%08ld",
               atol(setting(ACONFIG_PARAM_NEXT_JOB, "1")));
      snprintf(out, size, "  %-*s: %s", LABEL_WIDTH, "Next job", value);
      return;
    case LIVE_FREE: {
      uint32_t total = 0;
      uint32_t free = 0;
      if (sdcard_getMountedInfo(&total, &free)) {
        snprintf(value, sizeof(value), "%lu MB of %lu MB", (unsigned long)free,
                 (unsigned long)total);
      } else {
        snprintf(value, sizeof(value), "no card");
      }
      snprintf(out, size, "  %-*s: %s", LABEL_WIDTH, "Free", value);
      return;
    }
    case LIVE_USB:
      if (usbvol_mounted()) {
        snprintf(value, sizeof(value), "connected, %lu PDFs",
                 (unsigned long)usbvol_getCounters()->pdfs);
      } else {
        snprintf(value, sizeof(value), "not connected");
      }
      snprintf(out, size, "  %-*s: %s", LABEL_WIDTH, "PDFPRINTER", value);
      return;
    case LIVE_MACHINE:
      machine(value, sizeof(value));
      snprintf(out, size, "  %-*s: %s", LABEL_WIDTH, "Machine", value);
      return;
    case LIVE_SD:
      snprintf(out, size, "  %-*s: %s", LABEL_WIDTH, "Status",
               sdcard_isMounted() ? "mounted" : "no card");
      return;
    default:
      out[0] = '\0';
  }
}

// Redraws the live lines that changed; all of them when forced.
static void refresh(bool force) {
  bool drawn = false;
  for (int i = 0; i < LIVE_LINES; i++) {
    char text[TERM_SCREEN_SIZE_X];
    live_text(i, text, sizeof(text));
    if (!force && strcmp(text, shown[i]) == 0) continue;
    snprintf(shown[i], sizeof(shown[i]), "%s", text);
    row_text(liveRow[i], text);
    drawn = true;
  }
  if (drawn) {
    draw_icon(ROW_SD, ICON_HARD_DRIVE, sdcard_isMounted());
    draw_icon(ROW_USB, ICON_LIGHTBULB, usbvol_mounted());
    to_prompt();
    display_refresh();
  }
}

static void draw_settings(void) {
  char value[TERM_SCREEN_SIZE_X];
  row_item(ROW_PRINTER_KIND, "[P]rinter",
           laserjet() ? "HP LaserJet II" : "Epson FX-80");
  row_item(ROW_PAPER, "P[a]per", letter() ? "Letter" : "A4");
  snprintf(value, sizeof(value), "%d s",
           atoi(setting(ACONFIG_PARAM_IDLE_SECONDS, "10")));
  row_item(ROW_TIMEOUT, "[T]imeout", value);
  tail(value, VALUE_WIDTH + 1, setting(ACONFIG_PARAM_FOLDER, PRINTER_FOLDER));
  row_item(ROW_FOLDER, "F[o]lder", value);
}

void menu_title(void) {
  term_printString(
      "\x1B"
      "E"
      "\x1B"
      "p"
      "Printer Emulator - " RELEASE_VERSION
      "\n"
      "\x1B"
      "q");
}

void menu_show(void) {
  active = true;
  term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
  menu_title();
  row_text(ROW_PRINTER, "PRINTER");
  draw_settings();
  row_text(ROW_JOBS, "JOBS");
  row_text(ROW_SD, "SD CARD");
  row_text(ROW_USB, "USB DRIVE");
  row_text(ROW_ATARI, "ATARI");
  row_text(ROW_COMMANDS, "[E] Exit to desktop   [X] Booster");
  row_text(ROW_PROMPT, PROMPT);
  draw_divider(ROW_JOBS);
  draw_divider(ROW_SD);
  draw_divider(ROW_USB);
  draw_divider(ROW_ATARI);
  draw_icon(ROW_PRINTER, ICON_COG, true);
  refresh(true);
  draw_bottom();
  to_prompt();
  display_refresh();
  nextRefresh = make_timeout_time_ms(MENU_REFRESH_MS);
}

void menu_init(void) {
  term_setCommands(commands, sizeof(commands) / sizeof(commands[0]));
  term_clearScreen();
  countdown = MENU_COUNTDOWN_SECONDS;
  halted = false;
  restartOnHello = false;
  notice = NULL;
  (void)term_consumeAnyKeyPressed();
  nextTick = make_timeout_time_ms(1000);
  menu_show();
}

// Stops the countdown, with a message for the strip (NULL: the usual).
static void halt(const char *message) {
  halted = true;
  notice = message;
}

// ---- The keys -------------------------------------------------------------

static void cmdPrinter(const char *arg) {
  (void)arg;
  halt(NULL);
  settings_put_string(aconfig_getContext(), ACONFIG_PARAM_PRINTER,
                      laserjet() ? "FX80" : "LASERJET");
  save();
  menu_show();
}

static void cmdPaper(const char *arg) {
  (void)arg;
  halt(NULL);
  settings_put_string(aconfig_getContext(), ACONFIG_PARAM_PAPER,
                      letter() ? "A4" : "LETTER");
  save();
  menu_show();
}

// The idle timeouts T steps through, in seconds.
static const int timeouts[] = {5, 10, 20, 30, 60, 120};
#define TIMEOUTS ((int)(sizeof(timeouts) / sizeof(timeouts[0])))

// The next timeout of the list, after the first one, round again: a value
// not on the list goes to the next one above it.
static void cmdTimeout(const char *arg) {
  (void)arg;
  halt(NULL);
  int now = atoi(setting(ACONFIG_PARAM_IDLE_SECONDS, "10"));
  int next = timeouts[0];
  for (int i = 0; i < TIMEOUTS; i++) {
    if (timeouts[i] > now) {
      next = timeouts[i];
      break;
    }
  }
  settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_IDLE_SECONDS, next);
  if (save()) printer_readSettings();
  menu_show();
}

// ---- The folder picker ---------------------------------------------------
// md-drives-emulator's chooser: the card's folders, one level at a time; the
// cursor keys move, RETURN opens a folder, SPACE chooses the one shown, ESC
// leaves. Its list is only in RAM while it is open.

#define PICK_ENTRIES 64  // the folders listed at a level
#define PICK_ROWS 16     // a page's
#define PICK_TOP 3       // the list's first row
#define PICK_NAME SETTINGS_MAX_VALUE_LENGTH

typedef struct {
  char path[PICK_NAME];                 // the folder shown
  char names[PICK_ENTRIES][PICK_NAME];  // its folders, ".." first
  uint16_t count;
  uint16_t selected;
  bool cut;  // more folders than PICK_ENTRIES
} Picker;

static Picker *picker;

static int by_name(const void *a, const void *b) {
  return strcasecmp((const char *)a, (const char *)b);
}

// The folders in picker->path, in the order of their names.
static bool pick_load(void) {
  picker->count = 0;
  picker->selected = 0;
  picker->cut = false;
  if (strcmp(picker->path, "/") != 0) {
    snprintf(picker->names[picker->count++], PICK_NAME, "..");
  }
  uint16_t first = picker->count;
  DIR dir;
  FILINFO info;
  if (f_opendir(&dir, picker->path) != FR_OK) return false;
  while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0') {
    if (!(info.fattrib & AM_DIR) || info.fname[0] == '.') continue;
    if (picker->count == PICK_ENTRIES) {
      picker->cut = true;
      break;
    }
    snprintf(picker->names[picker->count++], PICK_NAME, "%s", info.fname);
  }
  f_closedir(&dir);
  qsort(picker->names[first], picker->count - first, PICK_NAME, by_name);
  return true;
}

static void pick_draw(void) {
  char line[TERM_SCREEN_SIZE_X];
  char where[TERM_SCREEN_SIZE_X];
  menu_title();
  tail(where, LINE_WIDTH - 9 + 1, picker->path);
  snprintf(line, sizeof(line), "Folder: %s", where);
  row_text(1, line);
  uint16_t page = picker->selected / PICK_ROWS;
  for (uint16_t k = 0; k < PICK_ROWS; k++) {
    uint16_t i = (uint16_t)(page * PICK_ROWS + k);
    if (i >= picker->count) break;
    snprintf(line, sizeof(line), "%c %s%s", i == picker->selected ? '>' : ' ',
             picker->names[i], strcmp(picker->names[i], "..") == 0 ? "" : "/");
    row_text((uint8_t)(PICK_TOP + k), line);
  }
  uint16_t pages = (uint16_t)((picker->count + PICK_ROWS - 1) / PICK_ROWS);
  snprintf(line, sizeof(line), "Page %u/%u%s", (unsigned)page + 1,
           (unsigned)(pages ? pages : 1),
           picker->cut ? ", more folders not shown" : "");
  row_text(PICK_TOP + PICK_ROWS + 1, line);
  row_text(ROW_COMMANDS, "Cursor keys move, RETURN opens a folder");
  row_text(ROW_PROMPT, "SPACE chooses it, ESC goes back");
  draw_strip(notice != NULL ? notice : "The folder the printer's PDFs go to.");
  notice = NULL;
  display_refresh();
}

static void pick_close(void) {
  free(picker);
  picker = NULL;
  term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
}

// Into the folder selected, or up for "..".
static void pick_open(void) {
  if (picker->count == 0) return;
  const char *name = picker->names[picker->selected];
  if (strcmp(name, "..") == 0) {
    char *slash = strrchr(picker->path, '/');
    if (slash == picker->path) {
      picker->path[1] = '\0';
    } else if (slash != NULL) {
      *slash = '\0';
    }
  } else {
    size_t length = strlen(picker->path);
    if (length + 1 + strlen(name) >= PICK_NAME) {
      notice = "That folder's path is too long for the setting.";
      return;
    }
    snprintf(picker->path + length, PICK_NAME - length, "%s%s",
             length > 1 ? "/" : "", name);
  }
  if (!pick_load()) notice = "The card could not list that folder.";
}

// The folder chosen: stored, and the jobs and the drive follow it.
static void use_folder(const char *path) {
  if (strcmp(path, "/") == 0) {
    notice = "Choose a folder, not the card's root.";
  } else if (printer_receiving() != 0 || pages_busy()) {
    notice = "A job is still printing: try again when it ends.";
  } else {
    settings_put_string(aconfig_getContext(), ACONFIG_PARAM_FOLDER, path);
    if (save()) {
      printer_readSettings();
      pages_init();
      usbvol_folderChanged();
    }
  }
}

static void cmdFolder(const char *arg) {
  halt(NULL);
  if (arg == NULL) {
    picker = calloc(1, sizeof(Picker));
    if (picker == NULL) {
      notice = "Not enough RAM for the folder picker.";
      menu_show();
      return;
    }
    // From the folder in use, or the root if it is not there.
    snprintf(picker->path, PICK_NAME, "%s",
             setting(ACONFIG_PARAM_FOLDER, PRINTER_FOLDER));
    if (!pick_load()) {
      snprintf(picker->path, PICK_NAME, "/");
      if (!pick_load()) {
        pick_close();
        notice = "The card could not list its folders.";
        menu_show();
        return;
      }
    }
    term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY_REENTRY);
    pick_draw();
    return;
  }
  uint16_t pageStart = (uint16_t)(picker->selected / PICK_ROWS * PICK_ROWS);
  switch (arg[0]) {
    case TERM_KEYBOARD_KEY_UP:
      if (picker->selected > 0) picker->selected--;
      break;
    case TERM_KEYBOARD_KEY_DOWN:
      if (picker->selected + 1 < picker->count) picker->selected++;
      break;
    case TERM_KEYBOARD_KEY_LEFT:
      picker->selected = pageStart >= PICK_ROWS ? pageStart - PICK_ROWS : 0;
      break;
    case TERM_KEYBOARD_KEY_RIGHT:
      if (pageStart + PICK_ROWS < picker->count) {
        picker->selected = (uint16_t)(pageStart + PICK_ROWS);
      }
      break;
    case '\r':
    case '\n':
      pick_open();
      break;
    case ' ': {
      char chosen[PICK_NAME];
      snprintf(chosen, sizeof(chosen), "%s", picker->path);
      pick_close();
      use_folder(chosen);
      menu_show();
      return;
    }
    case TERM_ESC_CHAR:
      pick_close();
      menu_show();
      return;
    default:
      return;
  }
  pick_draw();
}

static void cmdExit(const char *arg) {
  (void)arg;
  if (!emul_startPrinter()) {
    halt("Reset the Atari ST first: no hello from it yet.");
    draw_bottom();
    to_prompt();
    display_refresh();
    return;
  }
  active = false;
}

static void cmdBooster(const char *arg) {
  (void)arg;
  active = false;
  emul_startBooster();
}

// ---- The main loop ----------------------------------------------------------

void menu_loop(void) {
  if (!active) return;
  if (term_getCommandLevel() != TERM_COMMAND_LEVEL_SINGLE_KEY) return;
  if (!halted && term_consumeAnyKeyPressed()) {
    halt(NULL);
    draw_bottom();
    to_prompt();
    display_refresh();
  }
  absolute_time_t now = get_absolute_time();
  if (!halted && absolute_time_diff_us(nextTick, now) >= 0) {
    nextTick = make_timeout_time_ms(1000);
    if (--countdown <= 0) {
      countdown = MENU_COUNTDOWN_SECONDS;
      if (emul_startPrinter()) {
        active = false;
        return;
      }
      restartOnHello = true;
      halt("Reset the Atari ST: the printer starts after its hello.");
    }
    draw_bottom();
    to_prompt();
    display_refresh();
  }
  if (absolute_time_diff_us(nextRefresh, now) >= 0) {
    nextRefresh = make_timeout_time_ms(MENU_REFRESH_MS);
    refresh(false);
  }
}

void menu_stBooted(void) {
  if (restartOnHello) {
    restartOnHello = false;
    halted = false;
    notice = NULL;
    countdown = MENU_COUNTDOWN_SECONDS;
    nextTick = make_timeout_time_ms(1000);
    (void)term_consumeAnyKeyPressed();
  }
  if (active) menu_show();
}

void menu_stPublished(void) {
  if (active) refresh(false);
}
