#ifndef SR_H
#define SR_H

#define BIDIRECTIONAL 0

#include "emulator.h"  /* Required for struct msg and struct pkt definitions */

/* Function prototypes for Selective Repeat protocol */
void A_init(void);
void B_init(void);
void A_output(struct msg);
void A_input(struct pkt);
void A_timerinterrupt(void);

#endif
