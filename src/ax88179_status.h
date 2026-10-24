/* SPDX-License-Identifier: MIT
 * Private NETMAN ioctl shared between the EE app and the IOP module. */
#ifndef AX88179_STATUS_H
#define AX88179_STATUS_H
#define AX88179_IOCTL_STATUS 0x41580001u
#define AX88179_IOCTL_ENUMERATION 0x41580002u
#define AX88179_IOCTL_TRAFFIC 0x41580003u
#define AX88179_IOCTL_REGISTERS 0x41580004u
#define AX88179_IOCTL_PIPES 0x41580005u
enum {
    AX_WAITING = 0, AX_CONFIGURING, AX_READY, AX_LINK_UP,
    AX_FAILED, AX_DISCONNECTED, AX_STOPPED
};
typedef struct {
    int state;
    int usb_error;
    int stage;
    unsigned int phy_status;
    unsigned int rx_frames, tx_frames, rx_dropped, tx_dropped;
    unsigned int usb_devices_seen, probe_count;
    unsigned int usb_ids[4]; /* VID in high 16 bits, PID in low 16 bits. */
    unsigned int port_status[2]; /* Passive OHCI root-port register snapshot. */
} Ax88179Status;
/* NETMAN RPC has a fixed 64-byte output buffer. Never exceed it. */
typedef char Ax88179StatusFitsRpc[(sizeof(Ax88179Status) <= 64) ? 1 : -1];

typedef struct {
    unsigned int rx_completions, rx_bytes;
    int last_rx_size, last_rx_result;
    unsigned int last_rx_footer, rx_bad_batches, tx_submitted;
    int last_tx_size, last_tx_result, last_tx_count;
    unsigned char rx_header[12], tx_header[12]; /* Ethernet MACs only. */
} Ax88179Traffic;
typedef char Ax88179TrafficFitsRpc[(sizeof(Ax88179Traffic) <= 64) ? 1 : -1];

typedef struct {
    unsigned int valid, restarts, fifo_status, rx_control, medium, usb_status, clock;
    unsigned char bulk[5], node[6];
    unsigned int in_endpoint, out_endpoint, revision, configuration;
    unsigned int notifications, last_notification;
} Ax88179Registers;
typedef char Ax88179RegistersFitsRpc[(sizeof(Ax88179Registers) <= 64) ? 1 : -1];
#endif
