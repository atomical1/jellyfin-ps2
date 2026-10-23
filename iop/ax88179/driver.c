/* SPDX-License-Identifier: MIT
 * Experimental ASIX 0b95:1790 USB Ethernet driver for PS2 NETMAN.
 * Register definitions and initialization were checked against FreeBSD's
 * BSD-licensed if_axge driver; see docs/USB-ETHERNET.md for references.
 * Control and network work runs in our thread, never in a USBD callback.
 */
#include <irx.h>
#include <loadcore.h>
#include <usbd.h>
#include <netman.h>
#include <thbase.h>
#include <sysclib.h>
#include <errno.h>
#undef index /* sysclib aliases index(), but UsbDeviceRequest has an index field. */
#include "frames.h"
#include "ax88179_status.h"

IRX_ID("Jellyfin_AX88179", 1, 0);

typedef struct {
    volatile int done;
    int result, count;
} Completion;

static volatile int device = -1, disconnected, enabled;
static int control_pipe = -1, rx_pipe = -1, tx_pipe = -1, net_id = -1;
static int tx_max_packet, config_value, bcd_device;
static UsbEndpointDescriptor input_endpoint, output_endpoint;
static int link_up, link_mode = NETMAN_NETIF_ETH_LINK_MODE_AUTO;
static unsigned char mac[6];
static volatile Ax88179Status status;
static UsbDeviceRequest request;
static unsigned char control_buffer[64] __attribute__((aligned(64)));
static unsigned char rx_buffer[AX_RX_BUFFER_SIZE] __attribute__((aligned(64)));
static unsigned char tx_buffer[AX_FRAME_MAX + 16] __attribute__((aligned(64)));
static Completion control_io, rx_io, tx_io;
static int rx_pending, tx_pending;
static unsigned int tx_age;

static void complete(int result, int count, void *arg)
{
    Completion *io = arg;
    io->result = result;
    io->count = count;
    __asm__ volatile("" ::: "memory");
    io->done = 1;
}

static int control(unsigned char type, unsigned char command,
                   unsigned short value, unsigned short index,
                   void *data, unsigned int length)
{
    int rc, tries;
    if (disconnected || !enabled || length > sizeof(control_buffer)) return -1;
    request.requesttype = type;
    request.request = command;
    request.value = value;
    request.index = index;
    request.length = length;
    if (!(type & USB_DIR_IN) && length) memcpy(control_buffer, data, length);
    control_io.done = 0;
    rc = sceUsbdTransferPipe(control_pipe, length ? control_buffer : NULL,
                             length, &request, complete, &control_io);
    if (rc != USB_RC_OK) { status.usb_error = rc; return -1; }
    for (tries = 0; tries < 2000 && !control_io.done; ++tries) {
        if (disconnected || !enabled) return -1;
        DelayThread(1000);
    }
    if (!control_io.done) { status.usb_error = -1; return -1; }
    if (control_io.result != USB_RC_OK || control_io.count != (int)length) {
        status.usb_error = control_io.result ? control_io.result : -2;
        return -1;
    }
    if ((type & USB_DIR_IN) && length) memcpy(data, control_buffer, length);
    return 0;
}

static int write8(int reg, unsigned char value)
{
    return control(0x40, 1, reg, 1, &value, 1);
}

static int write16(int reg, unsigned short value)
{
    unsigned char data[2] = {value & 0xff, value >> 8};
    return control(0x40, 1, reg, 2, data, 2);
}

static int phy_read(int reg, unsigned short *value)
{
    unsigned char data[2];
    if (control(0xc0, 2, 3, reg, data, 2) < 0) return -1;
    *value = data[0] | ((unsigned short)data[1] << 8);
    return 0;
}

static int phy_write(int reg, unsigned short value)
{
    unsigned char data[2] = {value & 0xff, value >> 8};
    return control(0x40, 2, 3, reg, data, 2);
}

static int start_interface(void) { enabled = 1; return 0; }
static void stop_interface(void) { enabled = 0; }
static void transmit_available(void) { /* Worker polls the NETMAN queue. */ }

static int interface_ioctl(unsigned int command, void *args,
                           unsigned int args_len, void *out, unsigned int len)
{
    (void)args; (void)args_len;
    switch (command) {
    case AX88179_IOCTL_STATUS:
        if (!out || len < sizeof(Ax88179Status)) return -EINVAL;
        memcpy(out, (const void *)&status, sizeof(status)); return 0;
    case NETMAN_NETIF_IOCTL_ETH_GET_MAC:
        if (!out || len < 6) return -EINVAL;
        memcpy(out, mac, 6); return 0;
    case NETMAN_NETIF_IOCTL_GET_LINK_STATUS: return link_up;
    case NETMAN_NETIF_IOCTL_ETH_GET_LINK_MODE: return link_mode;
    case NETMAN_NETIF_IOCTL_GET_RX_DROPPED_COUNT: return status.rx_dropped;
    case NETMAN_NETIF_IOCTL_GET_TX_DROPPED_COUNT: return status.tx_dropped;
    case NETMAN_NETIF_IOCTL_ETH_GET_STATUS: {
        struct NetManEthStatus stats;
        if (!out || len < sizeof(stats)) return -EINVAL;
        memset(&stats, 0, sizeof(stats));
        stats.LinkStatus = link_up; stats.LinkMode = link_mode;
        stats.stats.RxDroppedFrameCount = status.rx_dropped;
        stats.stats.TxDroppedFrameCount = status.tx_dropped;
        memcpy(out, &stats, sizeof(stats)); return 0;
    }
    default:
        /* Returning success for SET_LINK_MODE without issuing a down event
         * would deadlock NetManSetLinkMode. PHY autonegotiation is automatic. */
        return -EINVAL;
    }
}

static struct NetManNetIF net_interface = {
    .name = "ax0", .flags = NETMAN_NETIF_ETHERNET,
    .init = start_interface, .deinit = stop_interface,
    .xmit = transmit_available, .ioctl = interface_ioctl
};

static void set_link(int up)
{
    if (up != link_up) {
        link_up = up;
        NetManToggleNetIFLinkState(net_id, up ?
            NETMAN_NETIF_ETH_LINK_STATE_UP : NETMAN_NETIF_ETH_LINK_STATE_DOWN);
    }
}

static void close_pipes(void)
{
    if (rx_pipe >= 0) sceUsbdClosePipe(rx_pipe);
    if (tx_pipe >= 0) sceUsbdClosePipe(tx_pipe);
    if (control_pipe >= 0) sceUsbdClosePipe(control_pipe);
    rx_pipe = tx_pipe = control_pipe = -1;
    /* Static buffers remain resident for any queued cancellation callbacks.
     * USB reattachment requires relaunching the ELF, so no stale operation
     * can complete into a buffer reused by another device. */
}

/* Called from the USBD connect callback while its cached descriptors are
 * stable. Copy the endpoint records so the worker never keeps a pointer
 * into memory that a concurrent USB disconnect can release. */
static int read_descriptors(int id)
{
    UsbConfigDescriptor *cfg;
    UsbDeviceDescriptor *dev;
    UsbEndpointDescriptor *in = NULL, *out = NULL;
    unsigned char *p, *end;
    int match = 0;
    dev = sceUsbdScanStaticDescriptor(id, NULL, USB_DT_DEVICE);
    cfg = sceUsbdScanStaticDescriptor(id, NULL, USB_DT_CONFIG);
    if (!dev || !cfg || cfg->wTotalLength < cfg->bLength || cfg->bLength < 9)
        return -1;
    bcd_device = dev->bcdDevice;
    config_value = cfg->bConfigurationValue;
    p = (unsigned char *)cfg + cfg->bLength;
    end = (unsigned char *)cfg + cfg->wTotalLength;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_DT_INTERFACE && p[0] >= 9) {
            UsbInterfaceDescriptor *iface = (UsbInterfaceDescriptor *)p;
            match = iface->bInterfaceNumber == 0 && iface->bAlternateSetting == 0;
        } else if (match && p[1] == USB_DT_ENDPOINT && p[0] >= 7) {
            UsbEndpointDescriptor *ep = (UsbEndpointDescriptor *)p;
            if ((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK) {
                if (ep->bEndpointAddress & USB_DIR_IN) in = ep;
                else out = ep;
            }
        }
        p += p[0];
    }
    if (!in || !out) return -1;
    tx_max_packet = out->wMaxPacketSizeLB | ((int)out->wMaxPacketSizeHB << 8);
    /* The PS2 is a full-speed OHCI host; do not claim high/super-speed endpoints. */
    if (tx_max_packet != 64 || in->wMaxPacketSizeLB != 64 || in->wMaxPacketSizeHB)
        return -1;
    memcpy(&input_endpoint, in, sizeof(input_endpoint));
    memcpy(&output_endpoint, out, sizeof(output_endpoint));
    return 0;
}

static int initialize_chip(void)
{
    unsigned short bmcr;
    unsigned int i, all_zero = 1, all_ff = 1;
    /* Full-speed timing with a 2 KiB aggregate limit. PS2 USBD can only
     * cover two memory pages per transfer, so the desktop 26 KiB buffer
     * must not be used. A complete normal Ethernet frame fits in 2 KiB. */
    unsigned char bulk[5] = {7, 0xcc, 0x4c, 0, 8};
    unsigned char zero = 0;
    status.stage = 1;
    control_pipe = sceUsbdOpenPipe(device, NULL);
    rx_pipe = sceUsbdOpenPipeAligned(device, &input_endpoint);
    tx_pipe = sceUsbdOpenPipeAligned(device, &output_endpoint);
    if (control_pipe < 0 || rx_pipe < 0 || tx_pipe < 0) return -1;
    if (control(0x00, USB_REQ_SET_CONFIGURATION, config_value, 0, NULL, 0) < 0)
        return -1;
    DelayThread(10000);
    status.stage = 2;
    if (write16(0x26, 0) < 0 || write16(0x26, 0x20) < 0) return -1;
    DelayThread(500000);
    if (write8(0x33, 3) < 0) return -1;
    DelayThread(200000);
    if (bcd_device >= 0x0200 && control(0x40, 8, 0, 1, &zero, 1) < 0) return -1;
    status.stage = 3;
    if (control(0xc0, 1, 0x10, 6, mac, 6) < 0) return -1;
    for (i = 0; i < 6; ++i) { if (mac[i]) all_zero = 0; if (mac[i] != 0xff) all_ff = 0; }
    if (all_zero || all_ff || (mac[0] & 1)) return -1;
    if (control(0x40, 1, 0x2e, 5, bulk, 5) < 0 ||
        write8(0x54, 0x52) < 0 || write8(0x55, 0x34) < 0 ||
        write8(0x34, 0) < 0 || write8(0x35, 0) < 0 || /* No checksum offload. */
        write8(0x24, 0) < 0 || /* No wake-on-LAN. */
        write16(0x0b, 0x01aa) < 0) return -1; /* Receive, unicast/bcast/mcast, no IPE. */
    status.stage = 4;
    if (phy_read(0, &bmcr) < 0 || phy_write(0, (bmcr & ~0x0800) | 0x1200) < 0)
        return -1;
    status.state = AX_READY;
    return 0;
}

static int update_link(void)
{
    unsigned short phy, medium = 0x0130;
    if (phy_read(0x11, &phy) < 0) return -1;
    if (phy == status.phy_status) return 0;
    status.phy_status = phy;
    if (!(phy & 0x0400)) { set_link(0); status.state = AX_READY; return 0; }
    if (phy & 0x2000) medium |= 2;
    if ((phy & 0xc000) == 0x8000) medium |= 9;
    else if ((phy & 0xc000) == 0x4000) medium |= 0x0200;
    if (write16(0x22, medium) < 0) return -1;
    /* NETMAN has no gigabit enum; report the closest available mode. */
    if (phy & 0xc000) link_mode = (phy & 0x2000) ?
        NETMAN_NETIF_ETH_LINK_MODE_100M_FDX : NETMAN_NETIF_ETH_LINK_MODE_100M_HDX;
    else link_mode = (phy & 0x2000) ?
        NETMAN_NETIF_ETH_LINK_MODE_10M_FDX : NETMAN_NETIF_ETH_LINK_MODE_10M_HDX;
    status.state = AX_LINK_UP;
    set_link(1);
    return 0;
}

static void receive_frame(const unsigned char *data, unsigned int size, void *arg)
{
    void *payload, *packet;
    (void)arg;
    if (!link_up || !enabled) return;
    packet = NetManNetProtStackAllocRxPacket(size, &payload);
    if (!packet) { ++status.rx_dropped; return; }
    memcpy(payload, data, size);
    NetManNetProtStackEnQRxPacket(packet);
    ++status.rx_frames;
}

static int service_receive(void)
{
    int rc;
    if (rx_pending && rx_io.done) {
        rx_pending = 0;
        if (rx_io.result != USB_RC_OK && rx_io.result != USB_RC_DATAUNDER) {
            status.usb_error = rx_io.result; return -1;
        }
        if (rx_io.count < 0 || rx_io.count > (int)sizeof(rx_buffer) ||
            ax_parse_rx(rx_buffer, rx_io.count, receive_frame, NULL) < 0)
            ++status.rx_dropped;
    }
    if (!rx_pending) {
        rx_io.done = 0;
        rc = sceUsbdTransferPipe(rx_pipe, rx_buffer, sizeof(rx_buffer), NULL,
                                 complete, &rx_io);
        if (rc != USB_RC_OK) { status.usb_error = rc; return -1; }
        rx_pending = 1;
    }
    return 0;
}

static int service_transmit(void)
{
    void *payload;
    int size, count, rc;
    if (tx_pending) {
        if (!tx_io.done) {
            if (++tx_age >= 5000) {
                ++status.tx_dropped; status.usb_error = -3; return -1;
            }
            return 0;
        }
        tx_pending = 0;
        if (tx_io.result != USB_RC_OK) {
            ++status.tx_dropped; status.usb_error = tx_io.result; return -1;
        }
        ++status.tx_frames;
    }
    if (!link_up || (size = NetManTxPacketNext(&payload)) <= 0) return 0;
    count = ax_build_tx(tx_buffer, sizeof(tx_buffer), payload, size, tx_max_packet);
    NetManTxPacketDeQ(); /* USB uses our copy, never NETMAN's queue buffer. */
    if (count < 0) { ++status.tx_dropped; return 0; }
    tx_io.done = 0;
    rc = sceUsbdTransferPipe(tx_pipe, tx_buffer, count, NULL, complete, &tx_io);
    if (rc != USB_RC_OK) { ++status.tx_dropped; status.usb_error = rc; return -1; }
    tx_pending = 1;
    tx_age = 0;
    return 0;
}

static void worker(void *arg)
{
    unsigned int ticks = 0;
    (void)arg;
    while (enabled && device < 0) DelayThread(10000);
    if (enabled && !disconnected) {
        status.state = AX_CONFIGURING;
        if (initialize_chip() < 0) goto failed;
        while (enabled && !disconnected) {
            status.stage = 5;
            if (service_receive() < 0 || service_transmit() < 0) goto failed;
            if (ticks++ % 250 == 0 && update_link() < 0) goto failed;
            DelayThread(1000);
        }
    }
    status.state = disconnected ? AX_DISCONNECTED : AX_STOPPED;
    goto done;
failed:
    status.state = disconnected ? AX_DISCONNECTED : AX_FAILED;
done:
    set_link(0);
    close_pipes();
    ExitThread();
}

static int probe(int id)
{
    UsbDeviceDescriptor *desc;
    if (device >= 0) return 0;
    desc = sceUsbdScanStaticDescriptor(id, NULL, USB_DT_DEVICE);
    return desc && desc->idVendor == 0x0b95 && desc->idProduct == 0x1790;
}

static int connect_device(int id)
{
    if (device >= 0) return 1;
    if (read_descriptors(id) < 0) {
        status.stage = 1; status.state = AX_FAILED; status.usb_error = -4;
        return 1;
    }
    __asm__ volatile("" ::: "memory");
    device = id;
    return 0;
}

static int disconnect_device(int id)
{
    if (id == device) disconnected = 1;
    return 0;
}

static sceUsbdLddOps usb_driver = {
    .name = "ax88179", .probe = probe, .connect = connect_device,
    .disconnect = disconnect_device
};

int _start(int argc, char **argv)
{
    iop_thread_t thread;
    int tid;
    (void)argc; (void)argv;
    memset(&thread, 0, sizeof(thread));
    thread.attr = TH_C;
    thread.thread = worker;
    thread.priority = 0x28;
    thread.stacksize = 0x1000;
    tid = CreateThread(&thread);
    if (tid < 0) return MODULE_NO_RESIDENT_END;
    net_id = NetManRegisterNetIF(&net_interface);
    if (net_id < 0) { DeleteThread(tid); return MODULE_NO_RESIDENT_END; }
    if (sceUsbdRegisterLdd(&usb_driver) != USB_RC_OK) {
        NetManUnregisterNetIF("ax0"); DeleteThread(tid); return MODULE_NO_RESIDENT_END;
    }
    if (StartThread(tid, NULL) < 0) {
        sceUsbdUnregisterLdd(&usb_driver);
        NetManUnregisterNetIF("ax0"); DeleteThread(tid); return MODULE_NO_RESIDENT_END;
    }
    return MODULE_RESIDENT_END;
}
