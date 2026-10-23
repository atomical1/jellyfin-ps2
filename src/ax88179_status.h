/* SPDX-License-Identifier: MIT
 * Private NETMAN ioctl shared between the EE app and the IOP module. */
#ifndef AX88179_STATUS_H
#define AX88179_STATUS_H
#define AX88179_IOCTL_STATUS 0x41580001u
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
} Ax88179Status;
#endif
