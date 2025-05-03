 #include <stdlib.h>
 #include <stdio.h>
 #include <stdbool.h>
 #include <string.h>          /* <--  memcpy / memset */
 #include "emulator.h"        /* simulator framework – defines struct msg / pkt */
 #include "sr.h"              /* function prototypes & compile‑time params  */
 
 /*--------------------------------------------------------------------*/
 /*                       Global data / statistics                     */
 /*--------------------------------------------------------------------*/
 
 #define SEQ_SPACE     8               /* sequence‑number space size   */
 #define WINDOW_SIZE   4               /* sender / receiver window     */
 
 static struct pkt A_buffer[SEQ_SPACE];/* sender’s pkt buffer          */
 static bool        A_valid[SEQ_SPACE];/* slot in use?                 */
 static int         A_base       = 0;  /* seq# of earliest unacked pkt */
 static int         A_nextseqnum = 0;  /* next sequence number to use  */
 static double      timeout_interval = 20.0; /* timer (sim units)      */
 
 static int         B_expected = 0;    /* next in‑order seq# at B      */
 static struct pkt  B_buffer[SEQ_SPACE];/* out‑of‑order storage        */
 static bool        B_valid[SEQ_SPACE];
 
 /* ---  statistics counters (avoid “undeclared” errors) -------------- */
 int window_full          = 0;  /* msgs dropped when window full    */
 int packets_resent       = 0;  /* retransmissions by A             */
 int total_ACKs_received  = 0;  /* all ACKs seen at A (incl. dup)   */
 int new_ACKs             = 0;  /* ACKs that newly ack data         */
 int packets_received     = 0;  /* pkts correctly delivered to B    */
 
 /*--------------------------------------------------------------------*/
 /*                         Helper  functions                          */
 /*--------------------------------------------------------------------*/
 
 /* Compute simple additive checksum (1‑complement handled by caller) */
 static int ComputeChecksum(struct pkt packet)
 {
     int sum = packet.seqnum + packet.acknum;
     for (int i = 0; i < 20; i++)
         sum += (unsigned char)packet.payload[i];
     return sum;
 }
 
 static bool IsCorrupted(struct pkt packet)
 {
     return packet.checksum != ComputeChecksum(packet);
 }
 
 /*--------------------------------------------------------------------*/
 /*                         Sender  (Entity A)                         */
 /*--------------------------------------------------------------------*/
 
 void A_output(struct msg message)
 {
     if (((A_nextseqnum + SEQ_SPACE) - A_base) % SEQ_SPACE >= WINDOW_SIZE) {
         /* window full ‑‑ drop message from layer‑5 */
         window_full++;
         return;
     }
 
     struct pkt p;
     p.seqnum = A_nextseqnum;
     p.acknum = 0;
     memcpy(p.payload, message.data, sizeof p.payload);
     p.checksum = ComputeChecksum(p);
 
     A_buffer[A_nextseqnum] = p;
     A_valid [A_nextseqnum] = true;
 
     tolayer3(0, p);
 
     /* start timer if base equals nextseqnum (i.e. window was empty) */
     if (A_base == A_nextseqnum)
         starttimer(0, timeout_interval);
 
     A_nextseqnum = (A_nextseqnum + 1) % SEQ_SPACE;
 }
 
 void A_input(struct pkt packet)
 {
     if (IsCorrupted(packet))
         return;
 
     total_ACKs_received++;
 
     /* cumulative ACK – slide window */
     int ack = packet.acknum;
     while (A_base != (ack + 1) % SEQ_SPACE && A_valid[A_base]) {
         A_valid[A_base] = false;
         A_base = (A_base + 1) % SEQ_SPACE;
         new_ACKs++;
     }
 
     if (A_base == A_nextseqnum)
         stoptimer(0);              /* nothing outstanding  */
     else {
         stoptimer(0);
         starttimer(0, timeout_interval); /* restart timer  */
     }
 }
 
 void A_timerinterrupt(void)
 {
     starttimer(0, timeout_interval);   /* restart first!  */
 
     /* resend all un‑ACKed packets in current window */
     for (int i = 0; i < WINDOW_SIZE; i++) {
         int seq = (A_base + i) % SEQ_SPACE;
         if (A_valid[seq]) {
             tolayer3(0, A_buffer[seq]);
             packets_resent++;
         }
     }
 }
 
 void A_init(void)
 {
     memset(A_valid, 0, sizeof A_valid);
 }
 
 /*--------------------------------------------------------------------*/
 /*                       Receiver  (Entity B)                         */
 /*--------------------------------------------------------------------*/
 
 static void DeliverBufferedPackets(void)
 {
     /* deliver any now‑in‑order pkts */
     while (B_valid[B_expected]) {
         tolayer5(1, B_buffer[B_expected].payload);
         B_valid[B_expected] = false;
         B_expected = (B_expected + 1) % SEQ_SPACE;
         packets_received++;
     }
 }
 
 void B_input(struct pkt packet)
 {
     if (IsCorrupted(packet)) {                /* Bad packet – resend ACK */
         struct pkt nak;
         nak.seqnum = 0;
         nak.acknum = (B_expected + SEQ_SPACE - 1) % SEQ_SPACE;
         memset(nak.payload, 0, sizeof nak.payload);
         nak.checksum = ComputeChecksum(nak);
         tolayer3(1, nak);
         return;
     }
 
     /* In‑window and not yet received? */
     int diff = (packet.seqnum + SEQ_SPACE - B_expected) % SEQ_SPACE;
     bool in_window = diff < WINDOW_SIZE;
 
     if (in_window && !B_valid[packet.seqnum]) {
         /* buffer it */
         B_buffer[packet.seqnum] = packet;
         B_valid [packet.seqnum] = true;
     }
 
     /* ACK the highest contiguous seq# already received */
     struct pkt ack;
     ack.seqnum = 0;
     ack.acknum = (packet.seqnum + SEQ_SPACE) % SEQ_SPACE;
     memset(ack.payload, 0, sizeof ack.payload);
     ack.checksum = ComputeChecksum(ack);
     tolayer3(1, ack);
 
     DeliverBufferedPackets();
 }
 
 void B_init(void)
 {
     memset(B_valid, 0, sizeof B_valid);
 }
 
 /*--------------------------------------------------------------------*/
 /*                    Final statistics  printing                      */
 /*--------------------------------------------------------------------*/
 
 void Simulation_done(void)
 {
     printf("\n===== SR protocol statistics =====\n");
     printf("Messages dropped (window full):            %d\n", window_full);
     printf("Packets resent by A:                       %d\n", packets_resent);
     printf("Total ACKs received at A:                  %d\n", total_ACKs_received);
     printf("New ACKs that advanced window:             %d\n", new_ACKs);
     printf("Correct packets received at B:             %d\n", packets_received);
     printf("===========================================\n");
 }
 