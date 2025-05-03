#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "emulator.h"
#include "gbn.h"

/* ******************************************************************
   Go Back N protocol.  Adapted from J.F.Kurose
   ALTERNATING BIT AND GO-BACK-N NETWORK EMULATOR: VERSION 1.2

   Network properties:
   - one way network delay averages five time units (longer if there
   are other messages in the channel for GBN), but can be larger
   - packets can be corrupted (either the header or the data portion)
   or lost, according to user-defined probabilities
   - packets will be delivered in the order in which they were sent
   (although some can be lost).

   Modifications:
   - removed bidirectional GBN code and other code not used by prac.
   - fixed C style to adhere to current programming style
   - added GBN implementation
**********************************************************************/

#define RTT  16.0
#define WINDOWSIZE 6
#define SEQSPACE 12  // Sequence number space must be at least 2 * WINDOWSIZE
#define NOTINUSE (-1)

// Sender-side variables for Selective Repeat protocol
static struct pkt A_buffer[SEQSPACE];         // Buffer to store sent but unacknowledged packets
static bool A_acknowledged[SEQSPACE];         // Flags indicating which packets have been ACKed
static bool A_buffered[SEQSPACE];             // Flags indicating which packets are currently in use
static int A_base;                            // Base of the sending window
static int A_nextseqnum;                      // Next sequence number to be used for new packets
static int timer_seq = -1;                    // Sequence number currently being tracked by the timer

/* generic procedure to compute the checksum of a packet.  Used by both sender and receiver
   the simulator will overwrite part of your packet with 'z's.  It will not overwrite your
   original checksum.  This procedure must generate a different checksum to the original if
   the packet is corrupted.
*/
int ComputeChecksum(struct pkt packet)
{
  int checksum = 0;
  int i;

  checksum = packet.seqnum;
  checksum += packet.acknum;
  for ( i=0; i<20; i++ )
    checksum += (int)(packet.payload[i]);

  return checksum;
}

bool IsCorrupted(struct pkt packet)
{
  if (packet.checksum == ComputeChecksum(packet))
    return (false);
  else
    return (true);
}


/********* Sender (A) variables and functions ************/

static struct pkt buffer[WINDOWSIZE];  /* array for storing packets waiting for ACK */
static int windowfirst, windowlast;    /* array indexes of the first/last packet awaiting ACK */
static int windowcount;                /* the number of packets currently awaiting an ACK */
static int A_nextseqnum;               /* the next sequence number to be used by the sender */

/* called from layer 5 (application layer), passed the message to be sent to other side */
// This function is called whenever a new message is passed from layer 5 to layer 4 (sender side).
// It constructs a packet, stores it in the sender buffer, and sends it if the window is not full.

void A_output(struct msg message)
{
    // Check if window is full
    int window_size = (A_nextseqnum - A_base + SEQSPACE) % SEQSPACE;
    if (window_size >= WINDOWSIZE) {
        if (TRACE > 0)
            printf("A_output: Window full, dropping message.\n");
        window_full++;
        return;
    }

    // Construct the packet
    struct pkt newpkt;
    newpkt.seqnum = A_nextseqnum;
    newpkt.acknum = NOTINUSE;
    for (int i = 0; i < 20; i++) {
        newpkt.payload[i] = message.data[i];
    }
    newpkt.checksum = ComputeChecksum(newpkt);

    // Store packet in buffer
    A_buffer[A_nextseqnum] = newpkt;
    A_buffered[A_nextseqnum] = true;
    A_acknowledged[A_nextseqnum] = false;

    // Send packet to the network
    if (TRACE > 0)
        printf("A_output: Sending packet with seqnum %d\n", newpkt.seqnum);
    tolayer3(A, newpkt);

    // Start timer if this is the only unACKed packet (i.e. if no timer is running)
    if (timer_seq == -1) {
        starttimer(A, RTT);
        timer_seq = A_nextseqnum;
    }

    // Update next sequence number (wrap around SEQSPACE)
    A_nextseqnum = (A_nextseqnum + 1) % SEQSPACE;
}


/* called from layer 3, when a packet arrives for layer 4
   In this practical this will always be an ACK as B never sends data.
*/

void A_input(struct pkt packet)
{
    // Check if the ACK packet is corrupted
    if (IsCorrupted(packet)) {
        if (TRACE > 0)
            printf("A_input: Received corrupted ACK, ignoring.\n");
        return;
    }

    int acknum = packet.acknum;

    if (TRACE > 0)
        printf("A_input: Received ACK for seqnum %d\n", acknum);

    total_ACKs_received++;

    // Mark the packet as acknowledged
    if (!A_acknowledged[acknum]) {
        A_acknowledged[acknum] = true;
        new_ACKs++;
    }

    // If this ACK was for the packet currently being timed, restart timer
    if (acknum == timer_seq) {
        stoptimer(A);
        timer_seq = -1;

        // Search for the next earliest unACKed packet in the window
        for (int i = 0; i < WINDOWSIZE; i++) {
            int seq = (A_base + i) % SEQSPACE;
            if (A_buffered[seq] && !A_acknowledged[seq]) {
                starttimer(A, RTT);
                timer_seq = seq;
                break;
            }
        }
    }

    // Slide the window base forward for packets that have been acknowledged
    while (A_buffered[A_base] && A_acknowledged[A_base]) {
        A_buffered[A_base] = false;
        A_acknowledged[A_base] = false; // Optional: reset for reuse
        A_base = (A_base + 1) % SEQSPACE;
    }
}

/* called when A's timer goes off */

void A_timerinterrupt(void)
{
    if (timer_seq == -1 || !A_buffered[timer_seq] || A_acknowledged[timer_seq]) {
        if (TRACE > 0)
            printf("A_timerinterrupt: No valid packet to retransmit, timer_seq = %d\n", timer_seq);
        return;
    }

    // Retransmit the packet tracked by the timer
    struct pkt pkt_to_resend = A_buffer[timer_seq];
    if (TRACE > 0)
        printf("A_timerinterrupt: Timeout, resending packet seqnum %d\n", pkt_to_resend.seqnum);

    tolayer3(A, pkt_to_resend);
    packets_resent++;

    // Restart timer for the same packet
    starttimer(A, RTT);
}

/* the following routine will be called once (only) before any other */
/* entity A routines are called. You can use it to do any initialization */
void A_init(void)
{
    A_base = 0;           // Initialize base of the sender window
    A_nextseqnum = 0;     // Initialize next sequence number to use
    timer_seq = -1;       // No timer running at the beginning

    // Mark all packets as not yet sent and not acknowledged
    for (int i = 0; i < SEQSPACE; i++) {
        A_acknowledged[i] = false;
        A_buffered[i] = false;
    }
}



/********* Receiver (B)  variables and procedures ************/

static int expectedseqnum; /* the sequence number expected next by the receiver */
static int B_nextseqnum;   /* the sequence number for the next packets sent by B */


/* called from layer 3, when a packet arrives for layer 4 at B*/
void B_input(struct pkt packet)
{
  struct pkt sendpkt;
  int i;

  /* if not corrupted and received packet is in order */
  if  ( (!IsCorrupted(packet))  && (packet.seqnum == expectedseqnum) ) {
    if (TRACE > 0)
      printf("----B: packet %d is correctly received, send ACK!\n",packet.seqnum);
    packets_received++;

    /* deliver to receiving application */
    tolayer5(B, packet.payload);

    /* send an ACK for the received packet */
    sendpkt.acknum = expectedseqnum;

    /* update state variables */
    expectedseqnum = (expectedseqnum + 1) % SEQSPACE;
  }
  else {
    /* packet is corrupted or out of order resend last ACK */
    if (TRACE > 0)
      printf("----B: packet corrupted or not expected sequence number, resend ACK!\n");
    if (expectedseqnum == 0)
      sendpkt.acknum = SEQSPACE - 1;
    else
      sendpkt.acknum = expectedseqnum - 1;
  }

  /* create packet */
  sendpkt.seqnum = B_nextseqnum;
  B_nextseqnum = (B_nextseqnum + 1) % 2;

  /* we don't have any data to send.  fill payload with 0's */
  for ( i=0; i<20 ; i++ )
    sendpkt.payload[i] = '0';

  /* computer checksum */
  sendpkt.checksum = ComputeChecksum(sendpkt);

  /* send out packet */
  tolayer3 (B, sendpkt);
}

/* the following routine will be called once (only) before any other */
/* entity B routines are called. You can use it to do any initialization */
void B_init(void)
{
  expectedseqnum = 0;
  B_nextseqnum = 1;
}

/******************************************************************************
 * The following functions need be completed only for bi-directional messages *
 *****************************************************************************/

/* Note that with simplex transfer from a-to-B, there is no B_output() */
void B_output(struct msg message)
{
}

/* called when B's timer goes off */
void B_timerinterrupt(void)
{
}
