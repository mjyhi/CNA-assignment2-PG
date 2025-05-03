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
  int ackcount = 0;
  int i;

  /* if received ACK is not corrupted */
  if (!IsCorrupted(packet)) {
    if (TRACE > 0)
      printf("----A: uncorrupted ACK %d is received\n",packet.acknum);
    total_ACKs_received++;

    /* check if new ACK or duplicate */
    if (windowcount != 0) {
          int seqfirst = buffer[windowfirst].seqnum;
          int seqlast = buffer[windowlast].seqnum;
          /* check case when seqnum has and hasn't wrapped */
          if (((seqfirst <= seqlast) && (packet.acknum >= seqfirst && packet.acknum <= seqlast)) ||
              ((seqfirst > seqlast) && (packet.acknum >= seqfirst || packet.acknum <= seqlast))) {

            /* packet is a new ACK */
            if (TRACE > 0)
              printf("----A: ACK %d is not a duplicate\n",packet.acknum);
            new_ACKs++;

            /* cumulative acknowledgement - determine how many packets are ACKed */
            if (packet.acknum >= seqfirst)
              ackcount = packet.acknum + 1 - seqfirst;
            else
              ackcount = SEQSPACE - seqfirst + packet.acknum;

	    /* slide window by the number of packets ACKed */
            windowfirst = (windowfirst + ackcount) % WINDOWSIZE;

            /* delete the acked packets from window buffer */
            for (i=0; i<ackcount; i++)
              windowcount--;

	    /* start timer again if there are still more unacked packets in window */
            stoptimer(A);
            if (windowcount > 0)
              starttimer(A, RTT);

          }
        }
        else
          if (TRACE > 0)
        printf ("----A: duplicate ACK received, do nothing!\n");
  }
  else
    if (TRACE > 0)
      printf ("----A: corrupted ACK is received, do nothing!\n");
}

/* called when A's timer goes off */
void A_timerinterrupt(void)
{
  int i;

  if (TRACE > 0)
    printf("----A: time out,resend packets!\n");

  for(i=0; i<windowcount; i++) {

    if (TRACE > 0)
      printf ("---A: resending packet %d\n", (buffer[(windowfirst+i) % WINDOWSIZE]).seqnum);

    tolayer3(A,buffer[(windowfirst+i) % WINDOWSIZE]);
    packets_resent++;
    if (i==0) starttimer(A,RTT);
  }
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
