#ifndef SR_H
#define SR_H

#define BIDIRECTIONAL 0 
#define RTT 16.0
#define WINDOWSIZE 6
#define SEQSPACE 12
#define NOTINUSE (-1)

void A_output(struct msg message);
void A_input(struct pkt packet);
void A_timerinterrupt(void);
void A_init(void);

void B_input(struct pkt packet);
void B_init(void);
void B_output(struct msg message);
void B_timerinterrupt(void);

#endif
