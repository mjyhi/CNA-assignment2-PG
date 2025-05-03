/* ------------------------------------------------------------------
 * sr.h  ―  Selective Repeat protocol constants
 * ------------------------------------------------------------------*/
#ifndef SR_H
#define SR_H

#define RTT         16.0   /* timer value – MUST be 16.0 */
#define WINDOWSIZE  6      /* sender / receiver window size  */
#define SEQSPACE    12     /* sequence‑number space ≥ 2*WINDOWSIZE */
#define NOTINUSE   (-1)    /* placeholder for unused acknum */

#endif /* SR_H */
