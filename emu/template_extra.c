/**
 * File: template_extra.c
 * Description: What this firmware uses of the template's hardware layer and
 *              EmuMD's stand-ins do not provide, and what the emulated test
 *              harness (tools/dev/emu_harness.py) asks the firmware for.
 *
 *  - select_poll(), which the template's v1.3.0 added. SELECT is never
 *    pressed in EmuMD, so it only serves the harness's requests: the main
 *    loop calls it on every pass.
 *  - Requests, when EMU_REQUESTS names a folder: the harness creates a file
 *    there and the firmware answers and deletes it.
 *      text.req  writes text.txt: the setup terminal's screen as text, each
 *                cell of the framebuffer matched against the terminal's own
 *                font; a cell that matches no glyph is '?' ('#' when solid,
 *                the cursor).
 *      rom4.req  writes rom4.bin: the 64 KB cartridge window as the ST reads
 *                it at $FA0000 (big-endian words).
 *      svar.req  holds "INDEX VALUE" (hex): shared variable INDEX becomes
 *                VALUE, as the firmware's SET_SHARED_VAR writes it; answers
 *                svar.done. A test's way to flip a switch the firmware sets.
 *      setting.req  holds "KEY VALUE": the app's setting KEY becomes VALUE
 *                and the settings are saved, as the setup menu's put and save
 *                do; answers setting.done, which holds "ok" or why not.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aconfig.h"
#include "chandler.h"
#include "display.h"
#include "display_term.h"
#include "mdfw.h"

#define REQUEST_POLL_US 20000u
#define GLYPH_FIRST 32
#define GLYPH_LAST 126
#define GLYPH_ROWS 16 /* the scratch display: one tile wide, two high */
#define FB_STRIDE (DISPLAY_WIDTH / 8)

/* One glyph cell: the rows a scratch display holds, its baseline at
 * (DISPLAY_TERM_FIRST_ROW_OFFSET) * DISPLAY_TERM_CHAR_HEIGHT as the terminal's
 * first row has it, so scratch row k is framebuffer row
 * row * DISPLAY_TERM_CHAR_HEIGHT + k. */
static uint8_t glyphs[GLYPH_LAST - GLYPH_FIRST + 1][GLYPH_ROWS];
static int glyphTop = GLYPH_ROWS, glyphBottom = -1; /* rows any glyph uses */
static bool glyphsReady;

static const u8x8_display_info_t scratchInfo = {
    0, 1, 0, 0, 0, 0, 0, 0, 1000000UL, 0, 4, 0, 0,
    /* tile_width = */ 1,
    /* tile_height = */ GLYPH_ROWS / 8,
    /* default_x_offset = */ 0,
    /* flipmode_x_offset = */ 0,
    /* pixel_width = */ 8,
    /* pixel_height = */ GLYPH_ROWS};

static uint8_t scratchDisplay(u8x8_t *u8x8, uint8_t msg, uint8_t argInt,
                              void *argPtr) {
  (void)argInt;
  (void)argPtr;
  if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
    u8x8_d_helper_display_setup_memory(u8x8, &scratchInfo);
  }
  return 1;
}

static uint8_t scratchNothing(u8x8_t *u8x8, uint8_t msg, uint8_t argInt,
                              void *argPtr) {
  (void)u8x8;
  (void)msg;
  (void)argInt;
  (void)argPtr;
  return 1;
}

static void makeGlyphs(void) {
  static u8g2_t scratch;
  static uint8_t buffer[GLYPH_ROWS];
  u8g2_SetupDisplay(&scratch, scratchDisplay, scratchNothing, scratchNothing,
                    scratchNothing);
  u8g2_SetupBuffer(&scratch, buffer, GLYPH_ROWS / 8,
                   u8g2_ll_hvline_horizontal_right_lsb, U8G2_R0);
  u8g2_InitDisplay(&scratch);
  u8g2_SetFont(&scratch, u8g2_font_amstrad_cpc_extended_8f);
  for (int c = GLYPH_FIRST; c <= GLYPH_LAST; c++) {
    u8g2_ClearBuffer(&scratch);
    u8g2_DrawGlyph(&scratch, 0,
                   DISPLAY_TERM_FIRST_ROW_OFFSET * DISPLAY_TERM_CHAR_HEIGHT,
                   (uint16_t)c);
    memcpy(glyphs[c - GLYPH_FIRST], buffer, GLYPH_ROWS);
    for (int k = 0; k < GLYPH_ROWS; k++) {
      if (buffer[k]) {
        if (k < glyphTop) glyphTop = k;
        if (k > glyphBottom) glyphBottom = k;
      }
    }
  }
  glyphsReady = true;
}

static char cellChar(const uint8_t *fb, int col, int row) {
  int origin = row * DISPLAY_TERM_CHAR_HEIGHT;
  bool solid = true;
  for (int c = GLYPH_FIRST; c <= GLYPH_LAST; c++) {
    bool same = true;
    for (int k = glyphTop; k <= glyphBottom && same; k++) {
      same = fb[(origin + k) * FB_STRIDE + col] == glyphs[c - GLYPH_FIRST][k];
    }
    if (same) return (char)c;
  }
  for (int k = 0; k < DISPLAY_TERM_CHAR_HEIGHT; k++) {
    solid = solid && fb[(origin + k) * FB_STRIDE + col] == 0xFF;
  }
  return solid ? '#' : '?';
}

static void writeText(const char *path) {
  if (!glyphsReady) makeGlyphs();
  const uint8_t *fb = u8g2_GetBufferPtr(display_getU8g2Ref());
  FILE *f = fopen(path, "w");
  if (!f) return;
  for (int row = 0;
       row * DISPLAY_TERM_CHAR_HEIGHT + glyphBottom < DISPLAY_HEIGHT; row++) {
    char line[DISPLAY_TILES_WIDTH + 1];
    int end = 0;
    for (int col = 0; col < DISPLAY_TILES_WIDTH; col++) {
      line[col] = cellChar(fb, col, row);
      if (line[col] != ' ') end = col + 1;
    }
    line[end] = '\0';
    fprintf(f, "%s\n", line);
  }
  fclose(f);
}

static void writeRom4(const char *path) {
  const uint16_t *rom4 = mdfw_rom4();
  FILE *f = fopen(path, "wb");
  if (!f) return;
  for (int i = 0; i < 32768; i++) {
    uint8_t word[2] = {(uint8_t)(rom4[i] >> 8), (uint8_t)rom4[i]};
    fwrite(word, 1, 2, f);
  }
  fclose(f);
}

static const char *requestDir;

static void writeSharedVar(const char *path) {
  char req[1024];
  snprintf(req, sizeof(req), "%s/svar.req", requestDir);
  unsigned int index = 0, value = 0;
  FILE *f = fopen(req, "r");
  if (!f) return;
  int n = fscanf(f, "%x %x", &index, &value);
  fclose(f);
  if (n == 2 && index < CHANDLER_SHARED_VARIABLES_SLOTS) {
    uint16_t *rom4 = mdfw_rom4();
    unsigned int word = (CHANDLER_SHARED_VARIABLES_OFFSET + index * 4) / 2;
    rom4[word] = (uint16_t)(value >> 16);
    rom4[word + 1] = (uint16_t)value;
  }
  FILE *done = fopen(path, "w");
  if (done) fclose(done);
}

static void writeSetting(const char *path) {
  char req[1024];
  snprintf(req, sizeof(req), "%s/setting.req", requestDir);
  char key[SETTINGS_MAX_KEY_LENGTH] = {0};
  char value[SETTINGS_MAX_VALUE_LENGTH] = {0};
  const char *result = "no KEY VALUE";
  FILE *f = fopen(req, "r");
  if (f && fscanf(f, "%29s %95s", key, value) == 2) {
    SettingsContext *ctx = aconfig_getContext();
    SettingsConfigEntry *entry = settings_find_entry(ctx, key);
    int err = -1;
    if (!entry) {
      result = "no such setting";
    } else if (entry->dataType == SETTINGS_TYPE_BOOL) {
      err = settings_put_bool(ctx, key, strcmp(value, "true") == 0);
    } else if (entry->dataType == SETTINGS_TYPE_INT) {
      err = settings_put_integer(ctx, key, atoi(value));
    } else {
      err = settings_put_string(ctx, key, value);
    }
    if (entry) {
      result = (err == 0 && settings_save(ctx, true) >= 0) ? "ok" : "failed";
    }
  }
  if (f) fclose(f);
  FILE *done = fopen(path, "w");
  if (done) {
    fprintf(done, "%s\n", result);
    fclose(done);
  }
}

/* Answers one request: writes its answer under a temporary name, renames it,
 * then deletes the request, so the harness never reads half an answer. */
static void answer(const char *dir, const char *name, const char *out,
                   void (*write)(const char *)) {
  char req[1024], tmp[1024], dst[1024];
  snprintf(req, sizeof(req), "%s/%s", dir, name);
  if (access(req, F_OK) != 0) return;
  snprintf(tmp, sizeof(tmp), "%s/%s.tmp", dir, out);
  snprintf(dst, sizeof(dst), "%s/%s", dir, out);
  write(tmp);
  rename(tmp, dst);
  unlink(req);
}

void select_poll(void) {
  static const char *dir;
  static bool checked;
  static uint64_t next;
  if (!checked) {
    dir = getenv("EMU_REQUESTS");
    requestDir = dir;
    checked = true;
  }
  if (!dir) return;
  uint64_t now = mdfw_time_us();
  if (now < next) return;
  next = now + REQUEST_POLL_US;
  answer(dir, "text.req", "text.txt", writeText);
  answer(dir, "rom4.req", "rom4.bin", writeRom4);
  answer(dir, "svar.req", "svar.done", writeSharedVar);
  answer(dir, "setting.req", "setting.done", writeSetting);
}
