#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "emulator.h"
#include "sr.h"

/* ******************************************************************
   Go Back N protocol.  Adapted from J.F.Kurose
   ALTERNATING BIT AND GO-BACK-N NETWORK EMULATOR: VERSION 1.2
   ******************************************************************/

#define RTT  16.0
#define WINDOWSIZE 6
#define SEQSPACE 12  /* Sequence number space must be at least 2 * WINDOWSIZE */
#define NOTINUSE (-1)

/* Sender-side variables */
static struct pkt A_buffer[SEQSPACE];
static bool A_acknowledged[SEQSPACE];
static bool A_buffered[SEQSPACE];
static int A_base;
static int A_nextseqnum;
static int timer_seq = -1;

int ComputeChecksum(struct pkt packet) {
    int checksum = 0;
    int i;

    checksum = packet.seqnum;
    checksum += packet.acknum;
    for (i = 0; i < 20; i++) {
        checksum += (int)(packet.payload[i]);
    }

    return checksum;
}

bool IsCorrupted(struct pkt packet) {
    return packet.checksum != ComputeChecksum(packet);
}

/*
 * A_output: Called from layer 5 when the application wants to send a message.
 * If the sender's window is not full, it creates a packet and sends it.
 */
void A_output(struct msg message) {
    int window_size = (A_nextseqnum - A_base + SEQSPACE) % SEQSPACE;
    if (window_size >= WINDOWSIZE) {
        if (TRACE > 0)
            printf("----A: New message arrives, but window is full, dropping message");
        window_full++;
        return;
    }

    struct pkt newpkt;
    int i;
    newpkt.seqnum = A_nextseqnum;
    newpkt.acknum = NOTINUSE;
    for (i = 0; i < 20; i++) {
        newpkt.payload[i] = message.data[i];
    }
    newpkt.checksum = ComputeChecksum(newpkt);

    A_buffer[A_nextseqnum] = newpkt;
    A_buffered[A_nextseqnum] = true;
    A_acknowledged[A_nextseqnum] = false;

    if (TRACE > 0)
        printf("----A: New message arrives, send window is not full, send new messge to layer3! Sending packet %d to layer 3", newpkt.seqnum);
    tolayer3(A, newpkt);

    if (timer_seq == -1) {
        starttimer(A, RTT);
        timer_seq = A_nextseqnum;
    }

    A_nextseqnum = (A_nextseqnum + 1) % SEQSPACE;
}

/*
 * A_input: Called when an ACK is received at A.
 * Verifies the ACK, marks packet as acknowledged, and slides the window.
 */
void A_input(struct pkt packet) {
    if (IsCorrupted(packet)) {
        if (TRACE > 0)
            printf("----A: Received corrupted ACK, ignoring.");
        return;
    }

    int acknum = packet.acknum;
    if (TRACE > 0)
        printf("----A: uncorrupted ACK %d is received", acknum);

    total_ACKs_received++;

    if (!A_acknowledged[acknum]) {
        A_acknowledged[acknum] = true;
        new_ACKs++;
        if (TRACE > 0)
            printf("----A: ACK %d is not a duplicate", acknum);
    }

    if (acknum == timer_seq) {
        stoptimer(A);
        timer_seq = -1;

        int i;
        for (i = 0; i < WINDOWSIZE; i++) {
            int seq = (A_base + i) % SEQSPACE;
            if (A_buffered[seq] && !A_acknowledged[seq]) {
                starttimer(A, RTT);
                timer_seq = seq;
                break;
            }
        }
    }

    while (A_buffered[A_base] && A_acknowledged[A_base]) {
        A_buffered[A_base] = false;
        A_acknowledged[A_base] = false;
        A_base = (A_base + 1) % SEQSPACE;
    }
}

/*
 * A_timerinterrupt: Called when the timer expires at A.
 * Retransmits the corresponding packet and restarts the timer.
 */
void A_timerinterrupt(void) {
    if (timer_seq == -1 || !A_buffered[timer_seq] || A_acknowledged[timer_seq]) {
        if (TRACE > 0)
            printf("A_timerinterrupt: No valid packet to retransmit, timer_seq = %d", timer_seq);
        return;
    }

    struct pkt pkt_to_resend = A_buffer[timer_seq];
    if (TRACE > 0)
        printf("A_timerinterrupt: Timeout, resending packet seqnum %d", pkt_to_resend.seqnum);

    tolayer3(A, pkt_to_resend);
    packets_resent++;
    starttimer(A, RTT);
}

/*
 * A_init: Initializes all state for entity A.
 * Sets the base, next sequence number, and clears all buffers.
 */
void A_init(void) {
    int i;
    A_base = 0;
    A_nextseqnum = 0;
    timer_seq = -1;
    for (i = 0; i < SEQSPACE; i++) {
        A_acknowledged[i] = false;
        A_buffered[i] = false;
    }
}

/* Receiver-side */
static struct pkt B_buffer[SEQSPACE];
static bool B_received[SEQSPACE];
static int B_expectedseqnum = 0;

/*
 * B_input: Called when a packet arrives at B from the network.
 * If not corrupted and within window, buffer and deliver it to layer 5.
 * Sends an ACK regardless of packet validity.
 */
void B_input(struct pkt packet) {
    struct pkt ackpkt;
    int seqnum = packet.seqnum;

    if (IsCorrupted(packet)) {
        if (TRACE > 0)
            printf("----B: packet %d is corrupted, sending duplicate ACK!", (B_expectedseqnum - 1 + SEQSPACE) % SEQSPACE);
        goto send_ack;
    }

    int upper_window = (B_expectedseqnum + WINDOWSIZE) % SEQSPACE;
    bool in_window = (B_expectedseqnum <= seqnum && seqnum < B_expectedseqnum + WINDOWSIZE) ||
                     (B_expectedseqnum + WINDOWSIZE >= SEQSPACE && seqnum < upper_window);

    if (!in_window) {
        if (TRACE > 0)
            printf("----B: packet %d is outside the window, sending duplicate ACK!", seqnum);
        goto send_ack;
    }

    if (!B_received[seqnum]) {
        B_buffer[seqnum] = packet;
        B_received[seqnum] = true;
        packets_received++;
        if (TRACE > 0)
            printf("----B: packet %d is correctly received, send ACK!", seqnum);
    }

    while (B_received[B_expectedseqnum]) {
        tolayer5(B, B_buffer[B_expectedseqnum].payload);
        B_received[B_expectedseqnum] = false;
        B_expectedseqnum = (B_expectedseqnum + 1) % SEQSPACE;
    }

send_ack:
    ackpkt.seqnum = 0;
    ackpkt.acknum = seqnum;
    int i;
    for (i = 0; i < 20; i++) {
        ackpkt.payload[i] = '0';
    }
    ackpkt.checksum = ComputeChecksum(ackpkt);
    tolayer3(B, ackpkt);
    if (TRACE > 0)
        printf("----B: Sending ACK for seqnum %d", ackpkt.acknum);
}

/*
 * B_output: Not used in this unidirectional assignment.
 */
void B_output(struct msg message) { }
/*
 * B_timerinterrupt: Not used in this unidirectional assignment.
 */
void B_timerinterrupt(void) { }

/*
 * B_init: Initializes all state for entity B.
 */
void B_init(void) {
    int i;
    B_expectedseqnum = 0;
    for (i = 0; i < SEQSPACE; i++) {
        B_received[i] = false;
    }
}