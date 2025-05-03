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
#define IN_WINDOW(start, seq, size) (((seq - start + SEQSPACE) % SEQSPACE) < size)
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

    newpkt.seqnum = A_nextseqnum;
    newpkt.acknum = NOTINUSE;
    for (i = 0; i < 20; i++)
        newpkt.payload[i] = message.data[i];
    newpkt.checksum = ComputeChecksum(newpkt);

    A_buffer[A_nextseqnum] = newpkt;
    A_buffered[A_nextseqnum] = true;
    A_acknowledged[A_nextseqnum] = false;

    printf("----A: New message arrives, send window is not full, send new messge to layer3!\n");

    tolayer3(0, newpkt);

    if (timer_seq == -1) {
      for (i = 0; i < WINDOWSIZE; i++) {
          int seq = (A_base + i) % SEQSPACE;
          if (A_buffered[seq] && !A_acknowledged[seq]) {
              starttimer(0, RTT);
              timer_seq = seq;
              break;
          }
      }
    }
  
    A_nextseqnum = (A_nextseqnum + 1) % SEQSPACE;
}

void A_input(struct pkt packet) {
  int acknum = packet.acknum;
  int i;

  if (IsCorrupted(packet)) {
      printf("----A: corrupted ACK is received, do nothing!\n");
      return;
  }

  printf("----A: uncorrupted ACK %d is received\n", acknum);
  total_ACKs_received++;

  if (IN_WINDOW(A_base, acknum, WINDOWSIZE)) {
      if (!A_acknowledged[acknum]) {
          A_acknowledged[acknum] = true;
          new_ACKs++;
          printf("----A: ACK %d is not a duplicate\n", acknum);
      } else {
          printf("----A: duplicate ACK received, do nothing!\n");
      }

      while (A_buffered[A_base] && A_acknowledged[A_base]) {
          A_buffered[A_base] = false;
          A_acknowledged[A_base] = false;
          A_base = (A_base + 1) % SEQSPACE;
      }

      stoptimer(0);
      timer_seq = -1;
      for (i = 0; i < WINDOWSIZE; i++) {
          int seq = (A_base + i) % SEQSPACE;
          if (A_buffered[seq] && !A_acknowledged[seq]) {
              starttimer(0, RTT);
              timer_seq = seq;
              break;
          }
      }
  } else {
      printf("----A: ACK %d is outside window, ignoring\n", acknum);
  }
}

void A_timerinterrupt() {
  printf("----A: timeout, resend all unACKed packets in window\n");
  int i;

  stoptimer(0);
  timer_seq = -1;

  for (i = 0; i < WINDOWSIZE; i++) {
      int seq = (A_base + i) % SEQSPACE;
      if (A_buffered[seq] && !A_acknowledged[seq]) {
          printf("----A: resending packet %d\n", seq);
          tolayer3(0, A_buffer[seq]);
          packets_resent++;
      }
  }

  for (i = 0; i < WINDOWSIZE; i++) {
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
  total_ACKs_received = 0;
  new_ACKs = 0;
  window_full = 0;
  packets_resent = 0;

  for (i = 0; i < SEQSPACE; i++) {
      A_acknowledged[i] = false;
      A_buffered[i] = false;
      A_buffer[i].seqnum = 0;
      A_buffer[i].acknum = 0;
      A_buffer[i].checksum = 0;
      memset(A_buffer[i].payload, 0, sizeof(A_buffer[i].payload));
  }
}


static struct pkt B_buffer[SEQSPACE];
static bool B_received[SEQSPACE];
static int B_expectedseqnum = 0;
static int B_last_acknum = SEQSPACE - 1;

void B_input(struct pkt packet) {
    struct pkt ackpkt;
    int seqnum;
    int upper_window;
    int i;
    bool in_window;

    seqnum = packet.seqnum;
  
    upper_window = (B_expectedseqnum + WINDOWSIZE) % SEQSPACE;
    in_window = IN_WINDOW(B_expectedseqnum, seqnum, WINDOWSIZE);

    if (IsCorrupted(packet) || !in_window) {
        printf("----B: packet corrupted or not expected sequence number, resend ACK!\n");
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
      B_last_acknum = B_expectedseqnum; 
      B_expectedseqnum = (B_expectedseqnum + 1) % SEQSPACE;
    }
  

send_ack_only:
    ackpkt.seqnum = 0;
    if (!IsCorrupted(packet) && in_window && B_received[seqnum]) {
        ackpkt.acknum = seqnum;
        printf("----B: sending ACK %d (valid and in-window)\n", seqnum);
    } else {
        ackpkt.acknum = B_last_acknum;
        printf("----B: sending duplicate ACK %d (last in-order packet)\n", ackpkt.acknum);
    }
    for (i = 0; i < 20; i++)
        ackpkt.payload[i] = '0';
    ackpkt.checksum = ComputeChecksum(ackpkt);
    tolayer3(1, ackpkt);
}

void B_init(void) {
    int i;
    B_expectedseqnum = 0;
    for (i = 0; i < SEQSPACE; i++){
        B_received[i] = false;
        B_buffer[i].seqnum = 0;
        B_buffer[i].acknum = 0;
        B_buffer[i].checksum = 0;
        B_expectedseqnum = 0;
        memset(B_buffer[i].payload, 0, sizeof(B_buffer[i].payload));
    }
}

void B_output(struct msg message) {}
void B_timerinterrupt(void) {}