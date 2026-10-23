/* SPDX-License-Identifier: MIT */
#ifndef AX88179_FRAMES_H
#define AX88179_FRAMES_H

#define AX_FRAME_MAX 1514u
#define AX_RX_BUFFER_SIZE 2048u
typedef void (*ax_frame_sink)(const unsigned char *, unsigned int, void *);

/* No IPE padding: the hardware is configured with RCR_IPE cleared. */
int ax_parse_rx(const unsigned char *data, unsigned int size,
                ax_frame_sink sink, void *arg);
int ax_build_tx(unsigned char *out, unsigned int capacity,
                const unsigned char *frame, unsigned int size,
                unsigned int max_packet);
#endif
