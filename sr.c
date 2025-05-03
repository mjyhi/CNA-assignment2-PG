/* ========================================================================
 *  sr.c  ―  Selective‑Repeat protocol (C90‑compatible implementation)
 *           – drops messages from layer‑5 when the send window is full.
 *  All comments are in English; width ≤ 80 columns.
 * ====================================================================== */

 #include <stdlib.h>
 #include <stdio.h>
 #include <string.h>          /* memcpy / memset                      */
 #include "emulator.h"
 #include "sr.h"
 
 /* ----------------------------------------------------------------------
  *  Boolean type for pre‑C99 C compilers
  * -------------------------------------------------------------------- */
 typedef int  bool;
 #define true  1
 #define false 0
 
 /* ----------------------------------------------------------------------
  *  Helper macro
  * -------------------------------------------------------------------- */
 #define IN_WINDOW(base, seq) \
         ((((seq) - (base) + SEQSPACE) % SEQSPACE) < WINDOWSIZE)
 
 /* ----------------------------------------------------------------------
  *  External statistics / trace variables (defined in emulator.c)
  * -------------------------------------------------------------------- */
 extern int TRACE;
 extern int window_full;
 extern int packets_resent;
 extern int total_ACKs_received;
 extern int new_ACKs;
 extern int packets_received;
 
 /* ----------------------------------------------------------------------
  *  Utility functions
  * -------------------------------------------------------------------- */
 static int ComputeChecksum(struct pkt packet)
 {
     int i, sum;
 
     sum = packet.seqnum + packet.acknum;
     for (i = 0; i < 20; i++)
         sum += (unsigned char)packet.payload[i];
 
     return sum;
 }
 
 static bool IsCorrupted(struct pkt packet)
 {
     return (packet.checksum != ComputeChecksum(packet));
 }
 
 /* ----------------------------------------------------------------------
  *  Sender (entity A) state
  * -------------------------------------------------------------------- */
 static struct pkt A_buffer[SEQSPACE];       /* sent but not yet ACKed      */
 static bool       A_valid[SEQSPACE];        /* slot in use                 */
 static bool       A_acked[SEQSPACE];        /* packet already ACKed        */
 
 static int A_base       = 0;                /* left edge of send window    */
 static int A_nextseqnum = 0;                /* next sequence number to use */
 static int timer_seq    = -1;               /* seq# whose timer is running */
 
 /* Local helper : build + send a packet and (re)start timer if needed */
 static void A_send(struct msg message)
 {
     struct pkt p;
 
     memcpy(p.payload, message.data, 20);
     p.seqnum   = A_nextseqnum;
     p.acknum   = NOTINUSE;
     p.checksum = ComputeChecksum(p);
 
     A_buffer[A_nextseqnum] = p;
     A_valid[A_nextseqnum]  = true;
     A_acked[A_nextseqnum]  = false;
 
     if (TRACE > 1)
         printf("A: send pkt %d to layer3\n", p.seqnum);
 
     tolayer3(A, p);
 
     if (timer_seq == -1) {            /* no timer running               */
         starttimer(A, RTT);
         timer_seq = A_nextseqnum;
     }
 
     A_nextseqnum = (A_nextseqnum + 1) % SEQSPACE;
 }
 
 /* Called by layer‑5 on the sender side */
 void A_output(struct msg message)
 {
     int win_size = (A_nextseqnum - A_base + SEQSPACE) % SEQSPACE;
 
     if (win_size >= WINDOWSIZE) {      /* window full ‑‑ drop message   */
         window_full++;
         if (TRACE > 0)
             printf("A: window full, message dropped\n");
         return;
     }
 
     A_send(message);
 }
 
 /* Called when an ACK arrives from layer‑3 */
 void A_input(struct pkt packet)
 {
     int acknum, i;
 
     if (IsCorrupted(packet)) {
         if (TRACE > 0)
             printf("A: corrupted ACK ignored\n");
         return;
     }
 
     acknum = packet.acknum;
     total_ACKs_received++;
 
     if (!A_valid[acknum] || A_acked[acknum]) /* duplicate ACK            */
         return;
 
     A_acked[acknum] = true;
     new_ACKs++;
 
     /* slide window for every consecutive ACKed packet from the base    */
     while (A_valid[A_base] && A_acked[A_base]) {
         A_valid[A_base] = false;
         A_base = (A_base + 1) % SEQSPACE;
     }
 
     /* restart timer for earliest outstanding pkt, or stop if none      */
     stoptimer(A);
     timer_seq = -1;
     for (i = 0; i < WINDOWSIZE; i++) {
         int s = (A_base + i) % SEQSPACE;
         if (A_valid[s] && !A_acked[s]) {
             starttimer(A, RTT);
             timer_seq = s;
             break;
         }
     }
 }
 
 /* Timer expired – resend the timed packet */
 void A_timerinterrupt(void)
 {
     if (timer_seq == -1)               /* nothing to resend              */
         return;
 
     if (TRACE > 0)
         printf("A: timeout, resend pkt %d\n", timer_seq);
 
     tolayer3(A, A_buffer[timer_seq]);
     packets_resent++;
 
     starttimer(A, RTT);                /* restart timer for same pkt     */
 }
 
 void A_init(void)
 {
     int i;
 
     A_base       = 0;
     A_nextseqnum = 0;
     timer_seq    = -1;
 
     for (i = 0; i < SEQSPACE; i++) {
         A_valid[i] = false;
         A_acked[i] = false;
     }
 }
 
 /* ----------------------------------------------------------------------
  *  Receiver (entity B) state
  * -------------------------------------------------------------------- */
 static struct pkt B_buffer[SEQSPACE];   /* out‑of‑order packet storage    */
 static bool       B_valid[SEQSPACE];
 
 static int B_expected = 0;             /* next in‑order seq number       */
 
 /* deliver any buffered in‑order packets to layer‑5 */
 static void B_deliver(void)
 {
     while (B_valid[B_expected]) {
         tolayer5(B, B_buffer[B_expected].payload);
         B_valid[B_expected] = false;
         B_expected = (B_expected + 1) % SEQSPACE;
         packets_received++;
     }
 }
 
 /* Called when a data packet arrives from layer‑3 */
 void B_input(struct pkt packet)
 {
     int diff, i;
     bool in_window;
     struct pkt ack;
 
     if (IsCorrupted(packet)) {         /* send NAK for last good pkt     */
         ack.seqnum = 0;
         ack.acknum = (B_expected + SEQSPACE - 1) % SEQSPACE;
         memset(ack.payload, 0, 20);
         ack.checksum = ComputeChecksum(ack);
         tolayer3(B, ack);
         return;
     }
 
     diff       = (packet.seqnum - B_expected + SEQSPACE) % SEQSPACE;
     in_window  = (diff < WINDOWSIZE);
 
     if (in_window && !B_valid[packet.seqnum]) {
         B_buffer[packet.seqnum] = packet;
         B_valid[packet.seqnum]  = true;
     }
 
     /* ACK always piggy‑backs highest contiguous seq received so far    */
     ack.seqnum = 0;
     ack.acknum = (B_expected + SEQSPACE - 1) % SEQSPACE;
     memset(ack.payload, 0, 20);
     ack.checksum = ComputeChecksum(ack);
     tolayer3(B, ack);
 
     B_deliver();
 }
 
 void B_init(void)
 {
     memset(B_valid, 0, sizeof(B_valid));
 }
 
 /* -------- stubs required by the simulator in unidirectional mode ---- */
 void B_output(struct msg message)            { (void)message; /* unused */ }
 void B_timerinterrupt(void)                  { /* never used  */        }
 