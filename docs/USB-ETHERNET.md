# Experimental AX88179 USB Ethernet

This fork adds a PS2 IOP USB Ethernet driver for ASIX USB ID **0B95:1790**.
It was written for an adapter reporting hardware revision **0200** on Windows.
The driver switches revision 0200 and newer to the AX88179-compatible packet
format. Other USB IDs and unrelated USB Ethernet chipsets are not supported.

**Status: compiles and passes host packet tests; not yet tested on a real PS2.**
Successful compilation does not establish USB enumeration, DHCP, or playback.
The first console test may expose issues requiring another driver revision.

## Running

1. Keep `settings.txt` beside `jellyfin-ps2-usb-ax88179.elf` on a FAT32 USB stick.
2. Insert the stick and the ASIX adapter into the two front PS2 USB ports.
3. Connect the adapter to the router with an Ethernet cable before launching.
4. Start `mass:/jellyfin-ps2-usb-ax88179.elf` in uLaunchELF/wLaunchELF.
5. Confirm the splash identifies `usb-ax88179-experimental`, then press START
   on the connect screen. Use the server computer's local HTTP address.

The USB build loads NETMAN and the new AX88179 module. It does not load DEV9
or SMAP, so an HDD-only expansion-bay adapter can stay installed. No new
setting is required: `network=dhcp`, `video=480i`, and the existing server line
continue to apply. The PS2 SDK still names its logical TCP/IP interface `sm0`;
NETMAN calls the physical USB device `ax0`.

If there is no link, the error distinguishes no matching adapter, a USB
disconnect, a driver setup failure, and a detected adapter without Ethernet
link. Report the complete message, especially its step and USB result.

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

## Driver details

- The USB probe claims only 0B95:1790 with full-speed 64-byte bulk endpoints.
- Cached USB descriptors are copied during the connect callback. Transfers,
  PHY access, frame delivery and queue processing run in a dedicated thread.
- The controller is configured for 2 KiB receive aggregates. PS2 USBD limits
  each transfer to two memory pages; the desktop driver's 26 KiB full-speed
  buffer is unsuitable for this API.
- Hardware checksum offload and IP-alignment pseudoheaders are disabled.
  Frames are validated before delivery to NETMAN. lwIP computes checksums.
- Control transfers have a bounded wait. A stalled transmit eventually stops
  the interface. Static transfer buffers remain resident after cancellation.
- NETMAN's forced-link-mode API is skipped: this PHY negotiates automatically.
  Returning success without its required down event would block the caller.

Failure steps: 1 USB descriptors/configuration, 2 PHY power/clock, 3 MAC and
receive setup, 4 autonegotiation, 5 running transfers/link polling. USB results
are PS2 USBD error codes; negative private results mean timeout (-1), short
control reply (-2), stuck transmit (-3), or incompatible descriptors (-4).

## References and notices

The register protocol and initialization were checked against FreeBSD's
`if_axge.c` and `if_axgereg.h` (BSD-2-Clause), including its full-speed queue
configuration and the revision-0200 compatibility mode:

- https://github.com/freebsd/freebsd-src/blob/main/sys/dev/usb/net/if_axge.c
- https://github.com/freebsd/freebsd-src/blob/main/sys/dev/usb/net/if_axgereg.h
- https://github.com/ps2dev/ps2sdk/blob/master/NETMAN.txt
- https://github.com/ps2dev/ps2sdk/blob/master/iop/usb/usbd/include/usbd.h

See `third_party/AX88179-NOTICE.txt` for the FreeBSD attribution and license.
The app retains its original MIT license; the new packet helpers are MIT.
