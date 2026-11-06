/**
 * File: menu.h
 * Description: The setup menu the ST shows from the cartridge, in the look of
 *              md-devops: a title bar, the printer's settings, its jobs, the
 *              USB drive and the Atari, each a section with its icon, and on
 *              the bottom strip a countdown that starts the printer when it
 *              runs out (any key stops it).
 *
 * Keys, one each: P steps the printer through Auto, Epson FX-80 and HP
 * LaserJet II, A the paper through A4 and Letter, T the idle timeout through
 * 5 to 120 seconds; O picks the folder among the card's, in md-drives-
 * emulator's picker; E starts the printer and goes to the desktop, X goes
 * back to Booster. A change is stored at once and used from the next job.
 * The menu never deletes a PDF.
 */
#ifndef MENU_H
#define MENU_H

#include <stdbool.h>

#define MENU_COUNTDOWN_SECONDS 20
#define MENU_REFRESH_MS 500  // how often the live lines are looked at

/**
 * @brief The menu's keys go to the terminal; the menu is drawn.
 */
void menu_init(void);

/**
 * @brief Draws the whole menu, the countdown restarted.
 */
void menu_show(void);

/**
 * @brief The title bar on a cleared screen, for messages outside the menu.
 */
void menu_title(void);

/**
 * @brief One pass of the main loop: the countdown, and the live lines.
 */
void menu_loop(void);

/**
 * @brief The ST has booted again, or published its machine: the menu is
 * redrawn, and a countdown waiting for the ST's hello goes on.
 */
void menu_stBooted(void);
void menu_stPublished(void);

#endif  // MENU_H
