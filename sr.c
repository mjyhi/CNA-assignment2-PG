#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "emulator.h"
#include "sr.h"

/* ******************************************************************
   Selective Repeat Protocol
   Adapted from Go-Back-N example by Kurose & Ross
**********************************************************************/

#define RTT  16.0
#define WINDOWSIZE 6
#define SEQSPACE 12
#define BIDIRECTIONAL 0
#define NOTINUSE (-1)

static struct pkt A_buffer[SEQSPACE];
static bool A_acknowledged[SEQSPACE];
static bool A_buffered[SEQSPACE];
static int A_base;
static int A_nextseqnum;
static int timer_seq = -1;

int ComputeChecksum(struct pkt packet) {
    int checksum = packet.seqnum + packet.acknum;
    int i;
    for (i = 0; i < 20; i++)
        checksum += (int)(packet.payload[i]);
    return checksum;
}

bool IsCorrupted(struct pkt packet) {
    return packet.checksum != ComputeChecksum(packet);
}

void A_output(struct msg message) {
    struct pkt newpkt;
    int window_size = (A_nextseqnum - A_base + SEQSPACE) % SEQSPACE;
    int i;
    if (window_size >= WINDOWSIZE) {
        printf("----A: New message arrives, send window is full\n");
        window_full++;
        return;
    }

    printf("----A: New message arrives, send window is not full, send new messge to layer3!\n");

    newpkt.seqnum = A_nextseqnum;
    newpkt.acknum = NOTINUSE;
    for (i = 0; i < 20; i++)
        newpkt.payload[i] = message.data[i];
    newpkt.checksum = ComputeChecksum(newpkt);

    A_buffer[A_nextseqnum] = newpkt;
    A_buffered[A_nextseqnum] = true;
    A_acknowledged[A_nextseqnum] = false;

    tolayer3(0, newpkt);
    printf("Sending packet %d to layer 3\n", newpkt.seqnum);

    if (timer_seq == -1) {
        starttimer(0, RTT);
        timer_seq = A_nextseqnum;
        printf("          START TIMER: starting timer for packet %d\n", timer_seq);
    }

    A_nextseqnum = (A_nextseqnum + 1) % SEQSPACE;
}

void A_input(struct pkt packet) {
    int acknum;
    int i;
    if (IsCorrupted(packet)) {
        printf("----A: received corrupted ACK\n");
        return;
    }

    acknum = packet.acknum;
    printf("----A: uncorrupted ACK %d is received\n", acknum);
    total_ACKs_received++;

    if (!A_acknowledged[acknum]) {
        A_acknowledged[acknum] = true;
        new_ACKs++;

        printf("----A: ACK %d is not a duplicate\n", acknum);
    }

    if (acknum == timer_seq) {
        stoptimer(0);
        timer_seq = -1;

        for (i = 0; i < SEQSPACE; i++) {
            int seq = (A_base + i) % SEQSPACE;
            if (A_buffered[seq] && !A_acknowledged[seq]) {
                starttimer(0, RTT);
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

void A_timerinterrupt(void) {
  int i = 0;
  if (timer_seq == -1)
      return;

  if (A_buffered[timer_seq] && !A_acknowledged[timer_seq]) {
      printf("----A: timeout, resend packet %d\n", timer_seq);
      tolayer3(0, A_buffer[timer_seq]);
      packets_resent++;
  }

  stoptimer(0);
  timer_seq = -1;

  for (i = 0; i < SEQSPACE; i++) {
      int seq = (A_base + i) % SEQSPACE;
      if (A_buffered[seq] && !A_acknowledged[seq]) {
          starttimer(0, RTT);
          timer_seq = seq;
          break;
      }
  }
}


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

static struct pkt B_buffer[SEQSPACE];
static bool B_received[SEQSPACE];
static int B_expectedseqnum = 0;

void B_input(struct pkt packet) {
    struct pkt ackpkt;
    int seqnum;
    int upper_window;
    int i;
    bool in_window;

    seqnum = packet.seqnum;
    if (IsCorrupted(packet)) {
        printf("----B: received corrupted packet\n");
        goto send_ack_only;
    }
    upper_window = (B_expectedseqnum + WINDOWSIZE) % SEQSPACE;
    in_window = (B_expectedseqnum <= seqnum && seqnum < B_expectedseqnum + WINDOWSIZE) ||
                (B_expectedseqnum + WINDOWSIZE >= SEQSPACE && seqnum < upper_window);

    if (!in_window) {
        printf("----B: packet %d is outside the receive window\n", seqnum);
        goto send_ack_only;
    }

    if (!B_received[seqnum]) {
        B_buffer[seqnum] = packet;
        B_received[seqnum] = true;
        packets_received++;

        printf("----B: packet %d is correctly received, send ACK!\n", seqnum);
    }

    

    while (B_received[B_expectedseqnum]) {
        tolayer5(1, B_buffer[B_expectedseqnum].payload);
        B_received[B_expectedseqnum] = false;
        B_expectedseqnum = (B_expectedseqnum + 1) % SEQSPACE;
    }

send_ack_only:
    ackpkt.seqnum = 0;
    if (!IsCorrupted(packet) && B_received[seqnum]) {
        ackpkt.acknum = seqnum;
        printf("----B: sending ACK %d (received valid and in-order)\n", seqnum);
    } else {
        ackpkt.acknum = (B_expectedseqnum - 1 + SEQSPACE) % SEQSPACE;
        printf("----B: sending duplicate/lost ACK %d (last in-order packet)\n", ackpkt.acknum);
    }

    for (i = 0; i < 20; i++)
        ackpkt.payload[i] = '0';
    ackpkt.checksum = ComputeChecksum(ackpkt);
    tolayer3(1, ackpkt);
}

void B_init(void) {
    int i;
    B_expectedseqnum = 0;
    for (i = 0; i < SEQSPACE; i++)
        B_received[i] = false;
}

void B_output(struct msg message) {}
void B_timerinterrupt(void) {}