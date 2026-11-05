/**
 * File: pages.h
 * Description: The printer's jobs rendered into pages as an Epson FX-80
 *              prints them (fx80/fx80.h): a job is read back from the card
 *              once it has closed, a little at a time in the main loop, so
 *              receiving is never slowed. Debug builds log each page's size
 *              and the MD5 of its pixels, which tools/printer/render gives
 *              for the same job.
 */
#ifndef PAGES_H
#define PAGES_H

#include <stdint.h>

#define PAGES_QUEUE 4          // closed jobs waiting to be rendered
#define PAGES_BAND_ROWS 128    // rows of the page in RAM (fx80/raster.h)
#define PAGES_READ_SIZE 512    // bytes read from the card at a time
#define PAGES_BUDGET_US 1000   // rendering in one pass of the main loop
#define PAGES_FINISH_ROWS 32   // rows handed on at a time at a page's end

typedef struct {
  uint32_t jobs;     // jobs rendered
  uint32_t pages;    // pages rendered
  uint32_t dropped;  // jobs not rendered: the queue was full
  uint32_t failed;   // jobs not rendered: the card failed
  uint32_t lost;     // dots that fell above the band (fed back past it)
} PagesCounters;

/**
 * @brief A job's file has closed: it is rendered once the jobs before it
 * are.
 */
void pages_jobClosed(const char *path, uint32_t number);

/**
 * @brief One pass of the main loop: renders for at most PAGES_BUDGET_US.
 */
void pages_loop(void);

const PagesCounters *pages_getCounters(void);

#endif  // PAGES_H
