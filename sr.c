 #include <stdlib.h>
 #include <stdio.h>
 #include <string.h>      /* memset */
 #include "emulator.h"
 #include "sr.h"
 
 /* C90 has no <stdbool.h>; define a simple boolean type */
 typedef int bool;
 #define true  1
 #define false 0
 
 /* Return true if seq is inside the window that starts at start and has length size */
 #define IN_WINDOW(start,seq,size) \
         ( ((seq) - (start) + SEQSPACE) % SEQSPACE < (size) )
 
 /* ---------------- A‑side (sender) state ---------------- */
 static struct pkt A_buffer[SEQSPACE];
 static int        A_buffered[SEQSPACE];      /* 1 if occupied */
 static int        A_acknowledged[SEQSPACE];  /* 1 if ACKed */
 static int A_base;          /* first sequence number in window */
 static int A_nextseqnum;    /* next sequence number to use */
 static int timer_seq;       /* seq# currently timed, ‑1 = none */
 
 /* ---------------- B‑side (receiver) state ---------------- */
 static struct pkt B_buffer[SEQSPACE];
 static int        B_received[SEQSPACE];      /* 1 if packet already arrived */
 static int B_expectedseqnum;
 static int B_nextseqnum;
 
 /* Compute additive checksum of a packet – used by both sides */
int ComputeChecksum(struct pkt packet)
{
    int checksum = 0;
    int i;
    checksum += packet.seqnum;
    checksum += packet.acknum;
    for (i = 0; i < 20; i++)
        checksum += (int)packet.payload[i];
    return checksum;
}

/* Return true if packet is corrupted */
bool IsCorrupted(struct pkt packet)
{
    return packet.checksum != ComputeChecksum(packet);
}

/*------------------------------------------------------------------*/
/* A_output: called by layer‑5 when a new message is ready           */
void A_output(struct msg message)
{
    struct pkt sendpkt;
    int window_size;
    int i;

    window_size = (A_nextseqnum - A_base + SEQSPACE) % SEQSPACE;
    if (window_size >= WINDOWSIZE) {        /* window full, drop message */
        if (TRACE > 0)
            printf("----A: New message arrives, send window is full\n");
        window_full++;
        return;
    }

    /* Build the packet */
    sendpkt.seqnum  = A_nextseqnum;
    sendpkt.acknum  = NOTINUSE;
    for (i = 0; i < 20; i++)
        sendpkt.payload[i] = message.data[i];
    sendpkt.checksum = ComputeChecksum(sendpkt);

    /* Store in sender buffer */
    A_buffer[A_nextseqnum]      = sendpkt;
    A_buffered[A_nextseqnum]    = 1;
    A_acknowledged[A_nextseqnum]= 0;

    if (TRACE > 1)
        printf("----A: New message arrives, send window is not full, send new messge to layer3!\n");
    if (TRACE > 0)
        printf("Sending packet %d to layer 3\n", sendpkt.seqnum);

    tolayer3(A, sendpkt);

    /* Start timer if none is currently running */
    if (timer_seq == -1) {
        for (i = 0; i < WINDOWSIZE; i++) {
            int s = (A_base + i) % SEQSPACE;
            if (A_buffered[s] && !A_acknowledged[s]) {
                starttimer(A, RTT);
                timer_seq = s;
                break;
            }
        }
    }

    A_nextseqnum = (A_nextseqnum + 1) % SEQSPACE;
}

/*------------------------------------------------------------------*/
/* A_input: process an arriving ACK                                 */
void A_input(struct pkt packet)
{
    int acknum = packet.acknum;
    int i;

    if (!IsCorrupted(packet)) {
        if (TRACE > 0)
            printf("----A: uncorrupted ACK %d is received\n", acknum);
        total_ACKs_received++;

        if (A_buffered[acknum] && !A_acknowledged[acknum]) {
            /* This is a new ACK */
            A_acknowledged[acknum] = 1;
            new_ACKs++;

            /* Slide the send window as far as consecutive ACKs allow */
            while (A_buffered[A_base] && A_acknowledged[A_base]) {
                A_buffered[A_base] = 0;
                A_base = (A_base + 1) % SEQSPACE;
            }

            /* Restart timer for the oldest unACKed packet, if any */
            stoptimer(A);
            timer_seq = -1;
            for (i = 0; i < WINDOWSIZE; i++) {
                int s = (A_base + i) % SEQSPACE;
                if (A_buffered[s] && !A_acknowledged[s]) {
                    starttimer(A, RTT);
                    timer_seq = s;
                    break;
                }
            }
        } else {
            if (TRACE > 0)
                printf("----A: duplicate ACK received, do nothing!\n");
        }
    } else {
        if (TRACE > 0)
            printf("----A: corrupted ACK is received, do nothing!\n");
    }
}

/*------------------------------------------------------------------*/
/* A_timerinterrupt: timeout handler – retransmit unACKed packets   */
void A_timerinterrupt(void)
{
    int i;

    if (TRACE > 0)
        printf("----A: time out,resend packets!\n");

    stoptimer(A);
    timer_seq = -1;

    /* Resend every buffered but unACKed packet */
    for (i = 0; i < WINDOWSIZE; i++) {
        int s = (A_base + i) % SEQSPACE;
        if (A_buffered[s] && !A_acknowledged[s]) {
            if (TRACE > 0)
                printf("---A: resending packet %d\n", s);
            tolayer3(A, A_buffer[s]);
            packets_resent++;
            if (timer_seq == -1) {          /* restart timer on the earliest */
                starttimer(A, RTT);
                timer_seq = s;
            }
        }
    }
}

/*------------------------------------------------------------------*/
/* A_init: called once at simulator start                           */
void A_init(void)
{
    int i;
    A_base        = 0;
    A_nextseqnum  = 0;
    timer_seq     = -1;

    for (i = 0; i < SEQSPACE; i++) {
        A_buffered[i]     = 0;
        A_acknowledged[i] = 0;
        A_buffer[i].seqnum   = 0;
        A_buffer[i].acknum   = 0;
        A_buffer[i].checksum = 0;
        memset(A_buffer[i].payload, 0, sizeof(A_buffer[i].payload));
    }
}

/*------------------------------------------------------------------*/
/* B_input: handle an incoming data packet from A                    */
void B_input(struct pkt packet)
{
    struct pkt ackpkt;
    int i;

    /* ------------- 1. corruption check ------------- */
    if (IsCorrupted(packet)) {
        if (TRACE > 0)
            printf("----B: packet corrupted or not expected sequence number, resend ACK!\n");
        /* Re‑ACK the last in‑order packet */
        ackpkt.acknum = (B_expectedseqnum + SEQSPACE - 1) % SEQSPACE;
    }
    else if (!IN_WINDOW(B_expectedseqnum, packet.seqnum, WINDOWSIZE)) {
        /* ------------- 2. outside receive window ------------- */
        if (TRACE > 0)
            printf("----B: packet corrupted or not expected sequence number, resend ACK!\n");
        ackpkt.acknum = (B_expectedseqnum + SEQSPACE - 1) % SEQSPACE;
    }
    else {
        /* ------------- 3. packet is in receive window ------------- */
        if (!B_received[packet.seqnum]) {
            /* First time we see this seq – buffer it */
            B_buffer[packet.seqnum] = packet;
            B_received[packet.seqnum] = 1;
        }

        /* Send an ACK for *this* sequence number (Selective ACK) */
        if (TRACE > 0)
            printf("----B: packet %d is correctly received, send ACK!\n", packet.seqnum);
        packets_received++;
        ackpkt.acknum = packet.seqnum;

        /* ------------- 4. deliver any now‑in‑order data ------------- */
        while (B_received[B_expectedseqnum]) {
            /* pass payload up */
            tolayer5(B, B_buffer[B_expectedseqnum].payload);
            B_received[B_expectedseqnum] = 0;    /* mark slot free */
            B_expectedseqnum = (B_expectedseqnum + 1) % SEQSPACE;
        }
    }

    /* ------------- 5. build and send the ACK packet ------------- */
    ackpkt.seqnum = B_nextseqnum;        /* not used, but keep alternation */
    B_nextseqnum  = (B_nextseqnum + 1) % 2;

    for (i = 0; i < 20; i++)
        ackpkt.payload[i] = '0';
    ackpkt.checksum = ComputeChecksum(ackpkt);

    tolayer3(B, ackpkt);
}

/*------------------------------------------------------------------*/
void B_init(void)
{
    int i;
    B_expectedseqnum = 0;
    B_nextseqnum     = 1;

    for (i = 0; i < SEQSPACE; i++) {
        B_received[i] = 0;
        B_buffer[i].seqnum   = 0;
        B_buffer[i].acknum   = 0;
        B_buffer[i].checksum = 0;
        memset(B_buffer[i].payload, 0, sizeof(B_buffer[i].payload));
    }
}

/*------------------------------------------------------------------*/
/* Unidirectional data flow: no output from B to A and no timer     */
void B_output(struct msg message) { }
void B_timerinterrupt(void)      { }
