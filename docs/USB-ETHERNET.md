# Experimental AX88179 USB Ethernet

This fork adds a PS2 IOP USB Ethernet driver for ASIX USB ID **0B95:1790**.
It was written for an adapter reporting hardware revision **0200** on Windows.
The driver switches revision 0200 and newer to the AX88179-compatible packet
format. Other USB IDs and unrelated USB Ethernet chipsets are not supported.

**Status: build 5 console testing confirms USB enumeration, Ethernet link,
68 received frames, 7 USB sends, and DHCP reaching address-conflict checking.
Build 6 extends the DHCP wait. A bound address and playback remain unverified.**

## Running

1. Keep `settings.txt` beside `jellyfin-ps2-usb-ax88179.elf` on a FAT32 USB stick.
2. Insert the stick and the ASIX adapter into the two front PS2 USB ports.
3. Connect the adapter to the router with an Ethernet cable before launching.
4. Start `mass:/jellyfin-ps2-usb-ax88179.elf` in uLaunchELF/wLaunchELF.
5. Confirm the splash identifies `usb-ax88179-diag6`, then press START
   on the connect screen. Use the server computer's local HTTP address.

The USB build loads NETMAN and the new AX88179 module. It does not load DEV9
or SMAP, so an HDD-only expansion-bay adapter can stay installed. No new
setting is required: `network=dhcp`, `video=480i`, and the existing server line
continue to apply. The PS2 SDK still names its logical TCP/IP interface `sm0`;
NETMAN calls the physical USB device `ax0`.

If there is no link, the error distinguishes no matching adapter, a USB
disconnect, a driver setup failure, and a detected adapter without Ethernet
link. Report the complete message, especially its step and USB result.

Diagnostic build 2 loads the ASIX driver before USB mass storage and keyboard
drivers, and starts USBD with `conf=2048` instead of its 512-byte descriptor
cache. These address possible early device-detection failures; the original
hardware test reached the driver but reported no matching adapter.

If detection still fails, the error shows ready-device count, probe calls,
USB VID:PID values, and the two raw OHCI root-port status registers. A matching
`jfusbethernet.txt` is written beside settings.txt without any account tokens.
Reconnect the stick to the PC after that screen appears so the log can be read.
Diagnostic build 3 also embeds a corrected PS2SDK USBD module. Its original
multi-configuration fetch advanced the cached descriptor pointer but kept
writing into the old destination. A regression test reproduced configuration
1 writing to offset 18 instead of offset 57. The patch recomputes the
destination and reads lengths from bytes at odd addresses. The test now
passes with 1, 2, and 3 configurations under ASan/UBSan. This establishes a
host-code defect. The subsequent console test detected the adapter and reached
Ethernet link, then timed out waiting for DHCP.

Diagnostic build 4 also writes `jfusbethernet.txt` when DHCP fails. The error
shows driver state, step, USB error, and sent/received/dropped frame counts.
The log adds USB receive completions, byte counts, last receive footer and
parse failures, transmit submissions and completion results, Ethernet MAC
addresses and the TCP/IP interface's DHCP state. No packet payloads, account
tokens or credentials are recorded. These counters distinguish a silent
network path from transfers failing or arriving in an unexpected format.
The wall jack used in this test also works with the user's PC.

Build 5 leaves receive stopped during PHY autonegotiation, then restarts
the receive engine and drains the controller FIFO before announcing link
to NETMAN. The FIFO check is bounded; a persistently busy FIFO fails with
private error -5. The queue's zero-size setting now also zeros the batching
timer, matching ASIX's zero-size queue mode. The MAC is restored explicitly.
MAC register readback verifies receive and medium enables; missing enable
bits fail with error -6. The log includes actual receive-control, medium,
USB speed, clock, queue, node ID, endpoint and revision values. USB transmit
completion counts must match the submitted length. This is a candidate
receive-path correction. The next console log reported 69 receive completions,
68 delivered frames, no malformed batches, and DHCP state 8 (CHECKING),
establishing receive and DHCP reply processing on that run. One frame was
dropped. Register readback showed enabled receive, a clear FIFO and full-speed
USB. Hardware-normalized values were RX control 00AA, medium 0133, clock D9,
and queue 07:00:00:00:0A.

Build 6 gives the USB backend 30 seconds for DHCP instead of 12. If the client
is checking the offered address at the deadline, it allows 12 additional
seconds for the probes and announcements to finish. The wait remains bounded;
it does not bypass conflict detection or accept a pending address. The screen
reports DHCP phases and the log records elapsed seconds and state transitions.
Missing interfaces, DHCP OFF and BOUND with address zero are not accepted as
success. PS2SDK's getconfig failure value is zero, so these checks require its
documented success value of one. This addresses a likely premature timeout;
console confirmation of DHCP binding is still required.

The vendored module adds a read-only ABI 1.3 diagnostic extension while
retaining the original 1.2 API ordinals. The log includes non-ready devices'
enumeration state, last descriptor request, result, and configuration cursor.
Source provenance and local changes are in `third_party/ps2sdk-usbd/NOTICE.md`;
that component is Academic Free License 2.0, with the full license beside it.
Native SMAP builds continue to embed the original USB module from the image.

The status record is checked at compile time against NETMAN RPC's 64-byte
maximum. A port's low bit is physical connection; bit 1 is enable, bit 8 power,
and bit 9 low-speed mode. Reads do not alter port state.

USB disconnect is detected and pending pipes are closed. **Relaunch after
unplugging/reconnecting the USB adapter.** Ethernet cable reconnection is
polled automatically. The USB driver never writes EEPROM or permanent device
configuration. USB 1.1 bandwidth and IOP overhead can limit playback; video
throughput has not been measured on this driver.

## Building

Use the existing digest-pinned image in `docker-compose.yml`:

```sh
docker compose run --rm build -c 'make -j4 NETWORK=usb-ax88179'
```

Output: `jellyfin-ps2-usb-ax88179.elf`. The driver IRX is embedded in that ELF;
there is no separate runtime IRX to install. `make NETWORK=smap` builds the
original native Ethernet variant. Backend changes invalidate affected objects.
Both variants have been cross-compiled with the project's pinned image.

Host packet tests (a C99 compiler and sanitizer runtime are required):

```sh
make -C tests
```

The tests cover aggregate bounds, dummy headers, CRC-damaged frames, invalid
lengths, all normal Ethernet TX lengths and USB short-packet padding, and
100,000 pseudorandom malformed batches under AddressSanitizer and UBSan.
They do not emulate the ASIX chip or validate the PS2 USB controller.
The same test command also runs the real vendored USB enumeration function
against one, two, and three configurations with odd-length descriptors.

## Driver details

- The USB probe claims only 0B95:1790 with full-speed 64-byte bulk endpoints.
- Cached USB descriptors are copied during the connect callback. Transfers,
  PHY access, frame delivery and queue processing run in a dedicated thread.
- The controller is configured for 2 KiB receive aggregates. PS2 USBD limits
  each transfer to two memory pages; the desktop driver's 26 KiB full-speed
  buffer is unsuitable for this API.
- Receive batching timers are zero in the zero-size queue mode. Link-up
  restarts receiving before allowing DHCP to send, and reads back MAC state.
- Hardware checksum offload and IP-alignment pseudoheaders are disabled.
  Frames are validated before delivery to NETMAN. lwIP computes checksums.
- Control transfers have a bounded wait. A stalled transmit eventually stops
  the interface. Static transfer buffers remain resident after cancellation.
- NETMAN's forced-link-mode API is skipped: this PHY negotiates automatically.
  Returning success without its required down event would block the caller.

Failure steps: 1 USB descriptors/configuration, 2 PHY power/clock, 3 MAC and
receive setup, 4 autonegotiation, 5 running transfers/link polling, 6 receive
restart/FIFO drain, 7 register readback. USB results
are PS2 USBD error codes; negative private results mean timeout (-1), short
control/data transfer (-2), stuck transmit (-3), incompatible descriptors (-4),
FIFO still busy (-5), or receive-enable readback failure (-6).

`tests/ax88179_link_test.py` exercises the actual receive-queue and restart
functions with a mock controller, covering operation order, all control
failure points, a busy FIFO, bounded retries and failed enable readback.
This verifies driver control flow, not the behavior of a physical ASIX chip.
`tests/dhcp_wait_test.py` exercises the actual DHCP wait functions against
delayed offers and conflict checks, including an offer near the deadline,
auto-mode grace, bounded failure, missing interface, client OFF and zero IP.

## References and notices

The register protocol and initialization were checked against FreeBSD's
`if_axge.c` and `if_axgereg.h` (BSD-2-Clause), including its full-speed queue
configuration and the revision-0200 compatibility mode:

- https://github.com/freebsd/freebsd-src/blob/main/sys/dev/usb/net/if_axge.c
- https://github.com/freebsd/freebsd-src/blob/main/sys/dev/usb/net/if_axgereg.h
- https://github.com/ps2dev/ps2sdk/blob/master/NETMAN.txt
- https://github.com/ps2dev/ps2sdk/blob/master/iop/usb/usbd/include/usbd.h

The receive stop/start and FIFO-status protocol was also checked against
ASIX's Linux driver; the register operations were independently implemented:

- https://github.com/torvalds/linux/blob/v6.12/drivers/net/usb/ax88179_178a.c
- https://android.googlesource.com/kernel/msm/+/a5c805aaf898b08538369b00e2f1dd7c6507e1d1/drivers/net/usb/ax88179_178a.c

DHCP state CHECKING and the conflict-check completion path were checked against
lwIP 2.2.1, which uses ACD probes and announcements before DHCP binding:

- https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/core/ipv4/dhcp.c
- https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/core/ipv4/acd.c

See `third_party/AX88179-NOTICE.txt` for the FreeBSD attribution and license.
The app retains its original MIT license; the new packet helpers are MIT.
