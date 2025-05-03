#ifndef SR_H
#define SR_H

// Function prototypes for Selective Repeat protocol

// Initializes sender-side variables (A)
void A_init(void);

// Initializes receiver-side variables (B)
void B_init(void);

// Called when the application layer sends a message (A)
void A_output(struct msg);

// Called when an ACK packet is received at the sender (A)
void A_input(struct pkt);

// Called when timer expires at the sender (A)
void A_timerinterrupt(void);

#endif
