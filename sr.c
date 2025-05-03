/* =========================================================================
 *  sr.c  ―  Reliable transport (Selective Repeat) implementation
 *           C90‑compliant; prints identical trace strings to gbn.c
 * =========================================================================*/

 #include <stdlib.h>
 #include <stdio.h>
 #include <string.h>          /* memset */
 
 #include "emulator.h"
 #include "sr.h"
 
 /* C90 has no <stdbool.h>; define a simple boolean type */
 typedef int bool;
 #define true  1
 #define false 0
 
 /* helper macro – true if seq is inside [start, start+size) modulo SEQSPACE */
 #define IN_WINDOW(start, seq, size) \
         ( ((seq) - (start) + SEQSPACE) % SEQSPACE < (size) )
 
 /* ------------------------------------------------------------------
  *  External statistics / trace variables (declared in emulator.c)
  * ------------------------------------------------------------------*/
 extern int TRACE;
 extern int packets_resent;
 extern int packets_received;
 extern int new_ACKs;
 extern int total_ACKs_received;
 extern int window_full;
 
 /* ------------------------------------------------------------------
  *  A‑side (sender) state
  * ------------------------------------------------------------------*/
 static struct pkt A_buffer[SEQSPACE];
 static int        A_buffered[SEQSPACE];      /* 1 if slot occupied        */
 static int        A_acknowledged[SEQSPACE];  /* 1 if ACK received         */
 static int A_base              = 0;          /* left edge of send window  */
 static int A_nextseqnum        = 0;          /* next seq# to allocate     */
 static int timer_seq           = -1;         /* seq# currently timed      */
 
 /* ---------- waiting queue for messages that arrive when window is full --- */
 static struct msg waiting_msg[SEQSPACE];
 static int queue_head = 0;   /* index of first waiting message            */
 static int queue_tail = 0;   /* one past last stored message              */
 static int queue_size = 0;
 
 /* queue helpers */
 static void EnqueueMessage(struct msg message)
 {
     waiting_msg[queue_tail] = message;
     queue_tail = (queue_tail + 1) % SEQSPACE;
     queue_size++;
 }
 static int DequeueMessage(struct msg *message)
 {
     if (queue_size == 0) return 0;
     *message = waiting_msg[queue_head];
     queue_head = (queue_head + 1) % SEQSPACE;
     queue_size--;
     return 1;
 }
 
 /* ------------------------------------------------------------------
  *  B‑side (receiver) state
  * ------------------------------------------------------------------*/
 static struct pkt B_buffer[SEQSPACE];
 static int        B_received[SEQSPACE];      /* 1 if packet arrived       */
 static int B_expectedseqnum = 0;             /* lowest not yet delivered  */
 static int B_nextseqnum     = 1;             /* just for trace consistency*/
 
 /* ------------------------------------------------------------------
  *  Utility helpers
  * ------------------------------------------------------------------*/
 static int ComputeChecksum(struct pkt packet)
 {
     int checksum = 0;
     int i;
     checksum += packet.seqnum;
     checksum += packet.acknum;
     for (i = 0; i < 20; i++)
         checksum += (int)packet.payload[i];
     return checksum;
 }
 static bool IsCorrupted(struct pkt packet)
 {
     return packet.checksum != ComputeChecksum(packet);
 }
 
 /* ==================================================================
  *  A‑side routines
  * ==================================================================*/
 
 /*------------------------------------------------------------------*/
 /* local helper – build/send packet given a message (window assumed
  * to have space) and start timer if needed                          */
 static void A_send_packet(struct msg message)
 {
     struct pkt sendpkt;
     int i;
 
     sendpkt.seqnum  = A_nextseqnum;
     sendpkt.acknum  = NOTINUSE;
     for (i = 0; i < 20; i++)
         sendpkt.payload[i] = message.data[i];
     sendpkt.checksum = ComputeChecksum(sendpkt);
 
     /* store in buffer */
     A_buffer[A_nextseqnum]      = sendpkt;
     A_buffered[A_nextseqnum]    = 1;
     A_acknowledged[A_nextseqnum]= 0;
 
     if (TRACE > 1)
         printf("----A: New message arrives, send window is not full, send new messge to layer3!\n");
     if (TRACE > 0)
         printf("Sending packet %d to layer 3\n", sendpkt.seqnum);
 
     tolayer3(A, sendpkt);
 
     /* start timer if none running */
     if (timer_seq == -1) {
         starttimer(A, RTT);
         timer_seq = A_nextseqnum;
     }
 
     A_nextseqnum = (A_nextseqnum + 1) % SEQSPACE;
 }
 
 /*------------------------------------------------------------------*/
 void A_output(struct msg message)
 {
     int window_size = (A_nextseqnum - A_base + SEQSPACE) % SEQSPACE;
     if (window_size >= WINDOWSIZE) {
         /* window full – queue the message instead of dropping */
         if (TRACE > 0)
             printf("----A: New message arrives, send window is full\n");
         window_full++;
         EnqueueMessage(message);
         return;
     }
 
     A_send_packet(message);
 }
 
 /*------------------------------------------------------------------*/
 void A_input(struct pkt packet)
 {
     int acknum = packet.acknum;
     int i;
 
     if (!IsCorrupted(packet)) {
         if (TRACE > 0)
             printf("----A: uncorrupted ACK %d is received\n", acknum);
         total_ACKs_received++;
 
         if (A_buffered[acknum] && !A_acknowledged[acknum]) {
             /* ---- new, non‑duplicate ACK ---- */
             if (TRACE > 0)
                 printf("----A: ACK %d is not a duplicate\n", acknum);
 
             A_acknowledged[acknum] = 1;
             new_ACKs++;
 
             /* slide window past consecutively ACKed packets */
             while (A_buffered[A_base] && A_acknowledged[A_base]) {
                 A_buffered[A_base] = 0;
                 A_base = (A_base + 1) % SEQSPACE;
             }
 
             /* restart timer for earliest outstanding packet */
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
 
     /* send waiting messages if window now has space */
     while (queue_size > 0 &&
            ((A_nextseqnum - A_base + SEQSPACE) % SEQSPACE) < WINDOWSIZE) {
         struct msg queued;
         DequeueMessage(&queued);
         A_send_packet(queued);
     }
 }
 
 /*------------------------------------------------------------------*/
 void A_timerinterrupt(void)
 {
     int i;
 
     if (TRACE > 0)
         printf("----A: time out,resend packets!\n");
 
     /* resend only the packet whose timer expired */
     if (timer_seq != -1 && A_buffered[timer_seq] && !A_acknowledged[timer_seq]) {
         if (TRACE > 0)
             printf("---A: resending packet %d\n", timer_seq);
         tolayer3(A, A_buffer[timer_seq]);
         packets_resent++;
     }
 
     /* restart timer for earliest outstanding packet */
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
 
     /* send queued messages if window has space */
     while (queue_size > 0 &&
            ((A_nextseqnum - A_base + SEQSPACE) % SEQSPACE) < WINDOWSIZE) {
         struct msg queued;
         DequeueMessage(&queued);
         A_send_packet(queued);
     }
 }
 
 /*------------------------------------------------------------------*/
 void A_init(void)
 {
     int i;
     A_base        = 0;
     A_nextseqnum  = 0;
     timer_seq     = -1;
     queue_head = queue_tail = queue_size = 0;
 
     for (i = 0; i < SEQSPACE; i++) {
         A_buffered[i]     = 0;
         A_acknowledged[i] = 0;
         A_buffer[i].seqnum   = 0;
         A_buffer[i].acknum   = 0;
         A_buffer[i].checksum = 0;
         memset(A_buffer[i].payload, 0, sizeof(A_buffer[i].payload));
 
         B_received[i] = 0;   /* receiver array also cleared here */
         B_buffer[i].seqnum   = 0;
         B_buffer[i].acknum   = 0;
         B_buffer[i].checksum = 0;
         memset(B_buffer[i].payload, 0, sizeof(B_buffer[i].payload));
     }
 }
 
 /* ==================================================================
  *  B‑side routines
  * ==================================================================*/
 
 /*------------------------------------------------------------------*/
 void B_input(struct pkt packet)
 {
     struct pkt ackpkt;
     int i;
 
     if (!IsCorrupted(packet) &&
         IN_WINDOW(B_expectedseqnum, packet.seqnum, WINDOWSIZE)) {
 
         /* first arrival of this seq? */
         if (!B_received[packet.seqnum]) {
             B_buffer[packet.seqnum] = packet;
             B_received[packet.seqnum] = 1;
         }
 
         if (TRACE > 0)
             printf("----B: packet %d is correctly received, send ACK!\n",
                    packet.seqnum);
         packets_received++;
 
         ackpkt.acknum = packet.seqnum;
 
         /* deliver any now‑in‑order packets */
         while (B_received[B_expectedseqnum]) {
             tolayer5(B, B_buffer[B_expectedseqnum].payload);
             B_received[B_expectedseqnum] = 0;      /* free slot */
             B_expectedseqnum = (B_expectedseqnum + 1) % SEQSPACE;
         }
     } else {
         if (TRACE > 0)
             printf("----B: packet corrupted or not expected sequence number, resend ACK!\n");
         /* re‑ACK last in‑order packet */
         ackpkt.acknum = (B_expectedseqnum + SEQSPACE - 1) % SEQSPACE;
     }
 
     /* build ACK packet */
     ackpkt.seqnum = B_nextseqnum;
     B_nextseqnum  = (B_nextseqnum + 1) % 2;
     for (i = 0; i < 20; i++)
         ackpkt.payload[i] = '0';
     ackpkt.checksum = ComputeChecksum(ackpkt);
 
     tolayer3(B, ackpkt);
 }
 
 /*------------------------------------------------------------------*/
 void B_init(void)
 {
     /* arrays already cleared in A_init for convenience */
     B_expectedseqnum = 0;
     B_nextseqnum     = 1;
 }
 
 /*------------------------------------------------------------------*/
 void B_output(struct msg message) { /* not used – unidirectional */ }
 void B_timerinterrupt(void)      { /* not used */ }
 