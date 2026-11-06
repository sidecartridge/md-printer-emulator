/**
 * File: term.c
 * Author: Diego Parrilla Santamaría
 * Date: January 2025, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS
 * Description: Online terminal
 */

#include "term.h"

#include "printer.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aconfig.h"
#include "chandler.h"
#include "constants.h"
#include "debug.h"
#include "display.h"
#include "display_term.h"
#include "gconfig.h"
#include "memfunc.h"
#include "pico/unique_id.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "tprotocol.h"

// Producer (term_command_cb, called from chandler_loop) and consumer
// (term_loop) both run sequentially on core 0, so the double-buffer swap
// doesn't need volatile or an IRQ-disable critical section.
static TransmissionProtocol protocolBuffers[2];
static uint8_t protocolReadIndex = 0;
static uint8_t protocolWriteIndex = 1;
static bool protocolBufferReady = false;
static uint32_t protocolOverwriteCount = 0;

// Command handlers
static void cmdClear(const char *arg);
static void cmdExit(const char *arg);
static void cmdHelp(const char *arg);
static void cmdUnknown(const char *arg);

// Command table
static const Command *commands;

// Number of commands in the table
static size_t numCommands = 0;

// Setter for commands and numCommands
void term_setCommands(const Command *cmds, size_t count) {
  commands = cmds;
  numCommands = count;
}

/**
 * @brief chandler callback that publishes a fully-parsed protocol command
 *        into the terminal's double-buffer for term_loop() to consume.
 *
 * Called from chandler_loop() on core 0 (or wherever the application drains
 * commemul). Copies the command into the inactive slot and atomically swaps
 * read/write roles. payloadPtr is provided by chandler (already advanced past
 * the random token) but term doesn't use it directly — it re-derives the
 * pointer from the published snapshot inside term_loop().
 */
void __not_in_flash_func(term_command_cb)(TransmissionProtocol *protocol,
                                          uint16_t *payloadPtr) {
  (void)payloadPtr;
  // The printer's blocks are printer.c's: copying each into the terminal's
  // buffer would cost 2 KB a block for nothing.
  if ((protocol->command_id & 0xFF00) == PRINTER_CMD_BASE) {
    return;
  }
  uint8_t writeIndex = protocolWriteIndex;
  TransmissionProtocol *writeBuffer = &protocolBuffers[writeIndex];

  // Copy the content of protocol to the active write buffer.
  writeBuffer->command_id = protocol->command_id;
  writeBuffer->payload_size = protocol->payload_size;
  writeBuffer->bytes_read = protocol->bytes_read;
  writeBuffer->final_checksum = protocol->final_checksum;

  // Sanity check: clamp payload_size to avoid overflow
  uint16_t size = protocol->payload_size;
  if (size > MAX_PROTOCOL_PAYLOAD_SIZE) {
    size = MAX_PROTOCOL_PAYLOAD_SIZE;
  }

  // Copy only used payload bytes
  memcpy(writeBuffer->payload, protocol->payload, size);

  if (protocolBufferReady) {
    protocolOverwriteCount++;
  }

  // Publish atomically by swapping read/write roles.
  uint8_t readIndex = protocolReadIndex;
  protocolReadIndex = writeIndex;
  protocolWriteIndex = readIndex;
  protocolBufferReady = true;
}

static char screen[TERM_SCREEN_SIZE];
static uint8_t cursorX = 0;
static uint8_t cursorY = 0;

// Store previous cursor position for block removal
static uint8_t prevCursorX = 0;
static uint8_t prevCursorY = 0;

// Buffer to keep track of chars entered between newlines
static char inputBuffer[TERM_INPUT_BUFFER_SIZE];
static size_t inputLength = 0;

// How a key is taken (term.h): a command of its own, a line, or a value.
static uint8_t commandLevel = TERM_COMMAND_LEVEL_COMMAND_INPUT;
static char lastSingleKeyCommand = 0;  // the command keys go back to
static bool anyKeyPressed = false;     // since term_consumeAnyKeyPressed

void term_setCommandLevel(uint8_t level) { commandLevel = level; }

uint8_t term_getCommandLevel(void) { return commandLevel; }

bool term_consumeAnyKeyPressed(void) {
  bool pressed = anyKeyPressed;
  anyKeyPressed = false;
  return pressed;
}

// The handler of a one-letter command, or NULL.
static void (*single_key_handler(char key))(const char *) {
  for (size_t i = 0; i < numCommands; i++) {
    if (commands[i].command[0] == key && commands[i].command[1] == '\0') {
      return commands[i].handler;
    }
  }
  return NULL;
}

// Clears entire screen buffer and resets cursor
void term_clearScreen(void) {
  memset(screen, 0, TERM_SCREEN_SIZE);
  cursorX = 0;
  cursorY = 0;
  display_termClear();
}

// Clears the input buffer
void term_clearInputBuffer(void) {
  memset(inputBuffer, 0, TERM_INPUT_BUFFER_SIZE);
  inputLength = 0;
}

// Custom scrollup function to scroll except for the last row
void termScrollupBuffer(uint16_t blankBytes) {
  // blank bytes is the number of bytes to blank out at the bottom of the
  // screen

  unsigned char *u8g2Buffer = u8g2_GetBufferPtr(display_getU8g2Ref());
  memmove(u8g2Buffer, u8g2Buffer + blankBytes,
          DISPLAY_BUFFER_SIZE - blankBytes -
              TERM_SCREEN_SIZE_X * DISPLAY_TERM_CHAR_HEIGHT);
  memset(u8g2Buffer + DISPLAY_BUFFER_SIZE - blankBytes -
             TERM_SCREEN_SIZE_X * DISPLAY_TERM_CHAR_HEIGHT,
         0, blankBytes);
}

// Scrolls the screen up by one row
static void termScrollUp(void) {
  memmove(screen, screen + TERM_SCREEN_SIZE_X,
          TERM_SCREEN_SIZE - TERM_SCREEN_SIZE_X);
  memset(screen + TERM_SCREEN_SIZE - TERM_SCREEN_SIZE_X, 0, TERM_SCREEN_SIZE_X);
  termScrollupBuffer(TERM_DISPLAY_ROW_BYTES);
}

// Prints a character to the screen, handles scrolling
static void termPutChar(char chr) {
  screen[cursorY * TERM_SCREEN_SIZE_X + cursorX] = chr;
  display_termChar(cursorX, cursorY, chr);
  cursorX++;
  if (cursorX >= TERM_SCREEN_SIZE_X) {
    cursorX = 0;
    cursorY++;
    if (cursorY >= TERM_SCREEN_SIZE_Y) {
      termScrollUp();
      cursorY = TERM_SCREEN_SIZE_Y - 1;
    }
  }
}

// Renders a single character, with special handling for newline and carriage
// return
static void termRenderChar(char chr) {
  // First, remove the old block by restoring the character
  display_termChar(prevCursorX, prevCursorY, ' ');
  if (chr == '\n' || chr == '\r') {
    // Move to new line
    cursorX = 0;
    cursorY++;
    if (cursorY >= TERM_SCREEN_SIZE_Y) {
      termScrollUp();
      cursorY = TERM_SCREEN_SIZE_Y - 1;
    }
  } else if (chr != '\0') {
    termPutChar(chr);
  }

  // Draw a block at the new cursor position
  display_termCursor(cursorX, cursorY);
  // Update previous cursor coords
  prevCursorX = cursorX;
  prevCursorY = cursorY;
}

/**
 * @brief Processes a complete VT52 escape sequence.
 *
 * This function interprets the VT52 sequence stored in `seq` (with given
 * length) and performs the corresponding cursor movements. Modify the TODO
 * sections to implement additional features as needed.
 *
 * @param seq Pointer to the escape sequence buffer.
 * @param length The length of the escape sequence.
 */
static void vt52ProcessSequence(const char *seq, size_t length) {
  // Ensure we have at least an ESC and a command character.
  if (length < 2) return;

  char command = seq[1];
  switch (command) {
    case 'A':  // Move cursor up
      // TODO(diego): Improve behavior if needed.
      if (cursorY > 0) {
        cursorY--;
      }
      termRenderChar('\0');
      break;
    case 'B':  // Move cursor down
      if (cursorY < TERM_SCREEN_SIZE_Y - 1) {
        cursorY++;
      }
      termRenderChar('\0');
      break;
    case 'C':  // Move cursor right
      if (cursorX < TERM_SCREEN_SIZE_X - 1) {
        cursorX++;
      }
      termRenderChar('\0');
      break;
    case 'D':  // Move cursor left
      if (cursorX > 0) {
        cursorX--;
      }
      termRenderChar('\0');
      break;
    case 'E':  // Clear screen and place cursor at top left corner
      cursorX = 0;
      cursorY = 0;
      termRenderChar('\0');
      for (int posY = 0; posY < TERM_SCREEN_SIZE_Y; posY++) {
        for (int posX = 0; posX < TERM_SCREEN_SIZE_X; posX++) {
          screen[posY * TERM_SCREEN_SIZE_X + posX] = 0;
          display_termChar(posX, posY, ' ');
        }
      }
      break;
    case 'H':  // Cursor home
      cursorX = 0;
      cursorY = 0;
      termRenderChar('\0');
      break;
    case 'J':  // Erases from the current cursor position to the end of the
               // screen
      for (int posY = cursorY; posY < TERM_SCREEN_SIZE_Y; posY++) {
        for (int posX = cursorX; posX < TERM_SCREEN_SIZE_X; posX++) {
          screen[posY * TERM_SCREEN_SIZE_X + posX] = 0;
          display_termChar(posX, posY, ' ');
        }
      }
      break;
    case 'K':  // Clear to end of line
      for (int posX = cursorX; posX < TERM_SCREEN_SIZE_X; posX++) {
        screen[cursorY * TERM_SCREEN_SIZE_X + posX] = 0;
        display_termChar(posX, cursorY, ' ');
      }
      break;
    case 'p':  // Reverse video: the row's background, then dark text
      u8g2_DrawBox(display_getU8g2Ref(), 0, cursorY * DISPLAY_TERM_CHAR_HEIGHT,
                   DISPLAY_WIDTH, DISPLAY_TERM_CHAR_HEIGHT);
      u8g2_SetDrawColor(display_getU8g2Ref(), 0);
      break;
    case 'q':  // Normal video
      u8g2_SetDrawColor(display_getU8g2Ref(), 1);
      break;
    case 'Y':  // Direct cursor addressing: ESC Y <row> <col>
      if (length == 4) {
        int row = seq[2] - TERM_POS_Y;
        int col = seq[3] - TERM_POS_X;
        if (row >= 0 && row < TERM_SCREEN_SIZE_Y && col >= 0 &&
            col < TERM_SCREEN_SIZE_X) {
          cursorY = row;
          cursorX = col;
        }
        termRenderChar('\0');
      }
      break;
    default:
      // Unrecognized sequence. Optionally, print or ignore.
      // For now, we'll ignore it.
      break;
  }
}

void term_printString(const char *str) {
  enum { STATE_NORMAL, STATE_ESC } state = STATE_NORMAL;
  char escBuffer[TERM_ESC_BUFFLINE_SIZE];
  size_t escLen = 0;

  while (*str) {
    char chr = *str;
    if (state == STATE_NORMAL) {
      if (chr == TERM_ESC_CHAR) {  // ESC character detected
        state = STATE_ESC;
        escLen = 0;
        escBuffer[escLen++] = chr;
      } else {
        termRenderChar(chr);
      }
    } else {  // STATE_ESC: we're accumulating an escape sequence
      escBuffer[escLen++] = chr;
      // Check for sequence completion:
      // Most VT52 sequences are two characters (ESC + command)...
      if (escLen == 2) {
        if (escBuffer[1] == 'Y') {
          // ESC Y requires two more characters (for row and col)
          // Do nothing now—wait until esc_len == 4.
        } else {
          // Sequence complete (ESC + single command)
          vt52ProcessSequence(escBuffer, escLen);
          state = STATE_NORMAL;
        }
      } else if (escBuffer[1] == 'Y' && escLen == 4) {
        // ESC Y <row> <col> sequence complete
        vt52ProcessSequence(escBuffer, escLen);
        state = STATE_NORMAL;
      }
      // In case the buffer gets too long, flush it as normal text
      if (escLen >= sizeof(escBuffer)) {
        for (size_t i = 0; i < escLen; i++) {
          termRenderChar(escBuffer[i]);
        }
        state = STATE_NORMAL;
      }
    }
    str++;
  }
  // If the string ends while still in ESC state, flush the buffered
  // characters as normal text.
  if (state == STATE_ESC) {
    for (size_t i = 0; i < escLen; i++) {
      termRenderChar(escBuffer[i]);
    }
  }
  display_termRefresh();
}

// Called whenever a character is entered by the user
// This is the single point of entry for user input
static void termInputChar(char chr) {
  anyKeyPressed = true;
  if (commandLevel == TERM_COMMAND_LEVEL_SINGLE_KEY) {
    // A key is a command: the menu's letters, either case.
    void (*handler)(const char *) =
        single_key_handler((char)tolower((unsigned char)chr));
    if (handler != NULL) {
      lastSingleKeyCommand = (char)tolower((unsigned char)chr);
      handler(NULL);
    }
    term_clearInputBuffer();
    display_termRefresh();
    return;
  }
  if (commandLevel == TERM_COMMAND_LEVEL_SINGLE_KEY_REENTRY) {
    // Each key, the cursor keys too, to the command that asked for them.
    void (*handler)(const char *) = single_key_handler(lastSingleKeyCommand);
    char key[2] = {chr, '\0'};
    if (handler != NULL) handler(key);
    display_termRefresh();
    return;
  }
  // Check for backspace
  if (chr == '\b') {
    display_termChar(prevCursorX, prevCursorY, ' ');

    // If we have chars in input_buffer, remove last char
    if (inputLength > 0) {
      inputLength--;
      inputBuffer[inputLength] = '\0';  // Null-terminate the string
      // Also reflect on screen
      // same as term_backspace
      if (cursorX == 0) {
        if (cursorY > 0) {
          cursorY--;
          cursorX = TERM_SCREEN_SIZE_X - 1;
        } else {
          // already top-left corner
          return;
        }
      } else {
        cursorX--;
      }
      screen[cursorY * TERM_SCREEN_SIZE_X + cursorX] = 0;
      display_termChar(cursorX, cursorY, ' ');
    }

    display_termCursor(cursorX, cursorY);
    prevCursorX = cursorX;
    prevCursorY = cursorY;
    display_termRefresh();
    return;
  }

  // If it's newline or carriage return, finalize the line
  if (chr == '\n' || chr == '\r') {
    // Render newline on screen
    termRenderChar('\n');

    // Process input_buffer
    // Split the input into command and argument
    char command[TERM_INPUT_BUFFER_SIZE] = {0};
    char arg[TERM_INPUT_BUFFER_SIZE] = {0};
    sscanf(inputBuffer, "%63s %63[^\n]", command,
           arg);  // Split at the first space

    bool commandFound = false;
    for (size_t i = 0; i < numCommands; i++) {
      if (strcmp(command, commands[i].command) == 0) {
        commands[i].handler(arg);  // Pass the argument to the handler
        commandFound = true;
      }
    }
    if ((!commandFound) && (strlen(command) > 0)) {
      // The custom unknown command manager is called when the command is empty
      // in the command table. This is useful to manage custom entries.
      for (size_t i = 0; i < numCommands; i++) {
        if (strlen(commands[i].command) == 0) {
          commands[i].handler(inputBuffer);  // Pass the argument to the handler
        }
      }
    }

    // Reset input buffer
    memset(inputBuffer, 0, TERM_INPUT_BUFFER_SIZE);
    inputLength = 0;

    term_printString("> ");
    display_termRefresh();
    return;
  }

  // If it's a normal character
  // Add it to input_buffer if there's space
  if (inputLength < TERM_INPUT_BUFFER_SIZE - 1) {
    inputBuffer[inputLength++] = chr;
    // Render char on screen
    termRenderChar(chr);

    // show block cursor

    display_termRefresh();
  } else {
    // Buffer full, ignore or beep?
  }
}

void term_init(void) {
  // Random-token publish, seed init, and shared-variable bookkeeping moved
  // to chandler (chandler_init / chandler_loop). term_init only handles
  // terminal display state now.

  // Initialize the welcome messages
  term_clearScreen();
  term_printString("Welcome to the terminal!\n");
  term_printString("Press ESC to enter the terminal.\n");
  term_printString("or any SHIFT key to boot the desktop.\n");

  // Example 1: Move the cursor up one line.
  // VT52 sequence: ESC A (moves cursor up)
  // The escape sequence "\x1BA" will move the cursor up one line.
  // term_printString("\x1B" "A");
  // After moving up, print text that overwrites part of the previous line.
  // term_printString("Line 2 (modified by ESC A)\n");

  // Example 2: Move the cursor right one character.
  // VT52 sequence: ESC C (moves cursor right)
  // term_printString("\x1B" "C");
  // term_printString(" <-- Moved right with ESC C\n");

  // Example 3: Direct cursor addressing.
  // VT52 direct addressing uses ESC Y <row> <col>, where:
  //   row_char = row + 0x20, col_char = col + 0x20.
  // For instance, to move the cursor to row 0, column 10:
  //   row: 0 -> 0x20 (' ')
  //   col: 10 -> 0x20 + 10 = 0x2A ('*')
  // term_printString("\x1B" "Y" "\x20" "\x2A");
  // term_printString("Text at row 0, column 10 via ESC Y\n");

  // term_printString("\x1B" "Y" "\x2A" "\x20");

  display_refresh();
}

// Invoke this function to process the commands from the active loop in the
// main function
void __not_in_flash_func(term_loop)() {
  // Read the published slot in place: a copy cost over 4 KB of stack, twice
  // the stack core 0 has. The double buffer keeps term_command_cb off this
  // slot until the next command after it, and everything is read from the
  // slot before termInputChar(), which can run a command handler that drains
  // commands again.
  const TransmissionProtocol *snapshot = NULL;
  bool protocolReady = false;
  uint32_t overwriteCountSnapshot = 0;

  if (protocolBufferReady) {
    snapshot = &protocolBuffers[protocolReadIndex];
    protocolBufferReady = false;
    protocolReady = true;
  }
  overwriteCountSnapshot = protocolOverwriteCount;

  if (protocolReady) {
    // Shared by all commands
    // Read the random token from the command and increment the payload
    // pointer to the first parameter available in the payload
    uint32_t randomToken = TPROTO_GET_RANDOM_TOKEN(snapshot->payload);
    uint16_t *payloadPtr = ((uint16_t *)snapshot->payload);
    uint16_t commandId = snapshot->command_id;
    DPRINTF(
        "Command ID: %d. Size: %d. Random token: 0x%08X, Checksum: 0x%04X, "
        "Overwrites: %lu\n",
        snapshot->command_id, snapshot->payload_size, randomToken,
        snapshot->final_checksum, (unsigned long)overwriteCountSnapshot);

#if defined(_DEBUG) && (_DEBUG != 0)
    // Jump the random token
    TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);

    // Read the payload parameters
    uint16_t payloadSizeTmp = 4;
    if ((snapshot->payload_size > payloadSizeTmp) &&
        (snapshot->payload_size <= TERM_PARAMETERS_MAX_SIZE)) {
      DPRINTF("Payload D3: 0x%04X\n", TPROTO_GET_PAYLOAD_PARAM32(payloadPtr));
      TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);
    }
    payloadSizeTmp += 4;
    if ((snapshot->payload_size > payloadSizeTmp) &&
        (snapshot->payload_size <= TERM_PARAMETERS_MAX_SIZE)) {
      DPRINTF("Payload D4: 0x%04X\n", TPROTO_GET_PAYLOAD_PARAM32(payloadPtr));
      TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);
    }
    payloadSizeTmp += 4;
    if ((snapshot->payload_size > payloadSizeTmp) &&
        (snapshot->payload_size <= TERM_PARAMETERS_MAX_SIZE)) {
      DPRINTF("Payload D5: 0x%04X\n", TPROTO_GET_PAYLOAD_PARAM32(payloadPtr));
      TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);
    }
    payloadSizeTmp += 4;
    if ((snapshot->payload_size > payloadSizeTmp) &&
        (snapshot->payload_size <= TERM_PARAMETERS_MAX_SIZE)) {
      DPRINTF("Payload D6: 0x%04X\n", TPROTO_GET_PAYLOAD_PARAM32(payloadPtr));
      TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);
    }
#endif

    // Handle the command
    switch (snapshot->command_id) {
      case APP_TERMINAL_START: {
        display_termStart(DISPLAY_TILES_WIDTH, DISPLAY_TILES_HEIGHT);
        term_clearScreen();
        term_printString("Type 'help' for available commands.\n");
        termInputChar('\n');
        SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_TERM);
        DPRINTF("Send command to display: DISPLAY_COMMAND_TERM\n");
      } break;
      case APP_TERMINAL_KEYSTROKE: {
        uint16_t *payload = ((uint16_t *)snapshot->payload);
        // Jump the random token
        TPROTO_NEXT32_PAYLOAD_PTR(payload);
        // Extract the 32 bit payload
        uint32_t payload32 = TPROTO_GET_PAYLOAD_PARAM32(payload);
        // Extract the ascii code from the payload lower 8 bits
        char keystroke = (char)(payload32 & TERM_KEYBOARD_KEY_MASK);
        // Get the shift key status from the higher byte of the payload
        uint8_t shiftKey =
            (payload32 & TERM_KEYBOARD_SHIFT_MASK) >> TERM_KEYBOARD_SHIFT_SHIFT;
        // Get the keyboard scan code from the bits 16 to 23 of the payload
        uint8_t scanCode =
            (payload32 & TERM_KEYBOARD_SCAN_MASK) >> TERM_KEYBOARD_SCAN_SHIFT;
        if (keystroke >= TERM_KEYBOARD_KEY_START &&
            keystroke <= TERM_KEYBOARD_KEY_END) {
          // Print the keystroke and the shift key status
          DPRINTF("Keystroke: %c. Shift key: %d, Scan code: %d\n", keystroke,
                  shiftKey, scanCode);
        } else {
          // Print the keystroke and the shift key status
          DPRINTF("Keystroke: %d. Shift key: %d, Scan code: %d\n", keystroke,
                  shiftKey, scanCode);
        }
        // The cursor keys, which have no character, by their scan codes.
        switch (scanCode) {
          case TERM_KEYBOARD_SCAN_CODE_UP:
            keystroke = TERM_KEYBOARD_KEY_UP;
            break;
          case TERM_KEYBOARD_SCAN_CODE_DOWN:
            keystroke = TERM_KEYBOARD_KEY_DOWN;
            break;
          case TERM_KEYBOARD_SCAN_CODE_LEFT:
            keystroke = TERM_KEYBOARD_KEY_LEFT;
            break;
          case TERM_KEYBOARD_SCAN_CODE_RIGHT:
            keystroke = TERM_KEYBOARD_KEY_RIGHT;
            break;
          default:
            break;
        }
        termInputChar(keystroke);
        break;
      }
      default:
        // Unknown command
        DPRINTF("Unknown command\n");
        break;
    }
    // Random-token publish is owned by chandler_loop; nothing more to do here.
    (void)randomToken;
  }
}

void term_cmdClear(const char *arg) { term_clearScreen(); }

void term_cmdExit(const char *arg) {
  term_printString("Exiting terminal...\n");
  // Send continue to desktop command
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_CONTINUE);
}

void term_cmdUnknown(const char *arg) {
  TPRINTF("Unknown command. Type 'help' for a list of commands.\n");
}
