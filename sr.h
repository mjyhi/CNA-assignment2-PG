/* ------------------------------------------------------------------
 * gbn.h ― Shared definitions for the Stop‑and‑Wait / Go‑Back‑N
 *         and Selective Repeat student implementations.
 *
 *  This header now contains:
 *    1. Constants required by the grader (RTT, WINDOWSIZE …)
 *    2. BIDIRECTIONAL flag expected by emulator.c
 *    3. Prototypes for A/B side callback functions
 * ------------------------------------------------------------------*/
#ifndef GBN_H
#define GBN_H

/* ====== Protocol‑level constants (MUST match assignment spec) ===== */
#define RTT         16.0   /* timer interval – must be 16.0 */
#define WINDOWSIZE  6      /* sender / receiver window size */
#define SEQSPACE    12     /* sequence‑number space ≥ 2×WINDOWSIZE */
#define NOTINUSE   (-1)    /* placeholder for unused acknum */

/* ====== Emulator flag ===== */
#define BIDIRECTIONAL 0    /* 0 = A→B only, 1 = both directions   */

/* ====== Function prototypes (called by emulator) ===== */
void A_init(void);
void B_init(void);

void A_output(struct msg message);
void B_output(struct msg message);      /* unused in unidirectional mode */

void A_input(struct pkt packet);
void B_input(struct pkt packet);

void A_timerinterrupt(void);
void B_timerinterrupt(void);

#endif /* GBN_H */
