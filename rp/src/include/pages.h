/**
 * File: pages.h
 * Description: The printer's jobs rendered into pages as an Epson FX-80
 *              prints them (fx80/fx80.h), or an HP LaserJet II (pcl/pcl.h),
 *              and written as a PDF (pdf/pdf.h): a job is read back from the
 *              card once it has closed, a little at a time in the main loop,
 *              so receiving is never slowed. The PRINTER setting names the
 *              printer; a job that starts in PCL goes to the LaserJet
 *              whatever it says.
 *              Debug builds log each page's size and the MD5 of its pixels,
 *              which tools/printer/render gives for the same job.
 *
 * A job's PDF is written as nnnnnnnn.PDT and renamed nnnnnnnn.pdf once
 * whole; the job, nnnnnnnn.PRN, is then deleted: it only waits to be
 * rendered. Jobs are rendered in the order of their numbers, and the folder
 * is never listed: NEXT_PDF, the next job to render, is stored with
 * NEXT_JOB. At boot each job from NEXT_PDF on that a reboot cut short is
 * finished: a job still under its temporary name is closed, a PDF not
 * finished is deleted, and a job beside its whole PDF is deleted. A job whose
 * PDF fails stays on the card.
 */
#ifndef PAGES_H
#define PAGES_H

#include <stdbool.h>
#include <stdint.h>

#define PAGES_PDF_EXTENSION ".pdf"
#define PAGES_PDF_TEMP_EXTENSION ".PDT"

#define PAGES_BAND_ROWS 128   // rows of the page in RAM (fx80/raster.h)
#define PAGES_READ_SIZE 512   // bytes read from the card at a time
#define PAGES_BUDGET_US 1000  // rendering in one pass of the main loop
#define PAGES_FINISH_ROWS 32  // rows handed on at a time at a page's end

typedef struct {
  uint32_t jobs;       // jobs rendered into a PDF
  uint32_t pages;      // pages rendered
  uint32_t failed;     // jobs not rendered: the card failed
  uint32_t lost;       // dots that fell above the band (fed back past it)
  uint32_t longestUs;  // the longest pass of pages_loop
  uint32_t lastJob;    // the last job rendered, 0 for none yet
  uint32_t lastPages;  // its pages: 0, no PDF
} PagesCounters;

/**
 * @brief The card is mounted: finishes the jobs a reboot cut short.
 */
void pages_init(void);

/**
 * @brief Job NUMBER has closed: it is rendered once the jobs before it are.
 */
void pages_jobClosed(uint32_t number);

/**
 * @brief One pass of the main loop: renders for at most PAGES_BUDGET_US.
 */
void pages_loop(void);

const PagesCounters *pages_getCounters(void);

/**
 * @brief The number of the job being rendered, or 0.
 */
uint32_t pages_rendering(void);

/**
 * @brief True while a job is being rendered or waits to be.
 */
bool pages_busy(void);

#endif  // PAGES_H
