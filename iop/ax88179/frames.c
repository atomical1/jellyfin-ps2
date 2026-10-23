/* SPDX-License-Identifier: MIT */
#include "frames.h"

static unsigned int le32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void put32(unsigned char *p, unsigned int value)
{
    unsigned int i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (i * 8));
}

int ax_parse_rx(const unsigned char *data, unsigned int size,
                ax_frame_sink sink, void *arg)
{
    unsigned int footer, count, headers, pos, i, pass;
    int delivered = 0;
    if (!data || size < 4 || size > AX_RX_BUFFER_SIZE) return -1;
    footer = le32(data + size - 4);
    count = footer & 0xffff;
    headers = footer >> 16;
    if (headers > size - 4 || count > (size - 4 - headers) / 4) return -1;

    /* Validate the whole batch before exposing any frame to the stack. */
    for (pass = 0; pass < 2; ++pass) {
        pos = 0;
        for (i = 0; i < count; ++i) {
            unsigned int status = le32(data + headers + i * 4);
            unsigned int length = (status >> 16) & 0x1fff;
            unsigned int padded = (length + 7) & ~7u;
            if (length == 0) continue; /* Newer firmware's dummy header. */
            if (pos > headers || padded > headers - pos) return -1;
            if (pass && !(status & 0xe0000000u) &&
                length >= 14 && length <= AX_FRAME_MAX) {
                if (sink) sink(data + pos, length, arg);
                ++delivered;
            }
            pos += padded;
        }
    }
    return delivered;
}

int ax_build_tx(unsigned char *out, unsigned int capacity,
                const unsigned char *frame, unsigned int size,
                unsigned int max_packet)
{
    unsigned int total, pad, i;
    if (!out || !frame || size < 14 || size > AX_FRAME_MAX ||
        !max_packet || max_packet > 512) return -1;
    total = size + 8;
    pad = total % max_packet == 0;
    if (capacity < total + pad) return -1;
    /* Disable per-frame checksum offload; lwIP supplies the checksums. */
    put32(out, size | 0x80000000u);
    put32(out + 4, pad ? 0x80008000u : 0);
    for (i = 0; i < size; ++i) out[i + 8] = frame[i];
    /* End the USB transfer with a short packet, as usbnet does. */
    if (pad) out[total++] = 0;
    return (int)total;
}
