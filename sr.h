#ifndef SR_H
#define SR_H

#include "emulator.h"

void A_output(struct msg message);
void A_input(struct pkt packet);
void A_timerinterrupt(void);
void A_init(void);

void B_output(struct msg message);
void B_input(struct pkt packet);
void B_timerinterrupt(void);
void B_init(void);

#endif