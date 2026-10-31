/**
 * File: printer.h
 * Description: What the RP shares with the ST's printer hook
 *              (target/atarist/src/inc/printer_layout.s): the switch that
 *              turns the hook on. With anything but PRINTER_ENABLED in shared
 *              variable PRINTER_SVAR_ENABLED, the hook hands every printer
 *              call to the routine it replaced.
 */
#ifndef PRINTER_H
#define PRINTER_H

#define PRINTER_SVAR_ENABLED 2         // shared variable index
#define PRINTER_ENABLED 0x50524F4Eu    // 'PRON'

#endif  // PRINTER_H
