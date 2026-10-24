# PS2SDK USB host driver

Source: https://github.com/ps2dev/ps2sdk/tree/329e2bb43287e16af16c4dde1c614e2662c24468/iop/usb/usbd

Copyright PS2DEV contributors. Licensed under the Academic Free License 2.0;
the full license is in LICENSE and original notices are retained in each file.

Local modification: `src/device.c`, `fetchConfigDescriptors`, recomputes the
destination after advancing to the next configuration. Descriptor lengths
are read from bytes to support configurations starting at odd addresses.
The original code reused the previous destination for each subsequent
configuration header. `tests/usbd_enumeration_test.py` exercises the actual
function from this source and reproduces that bug before the patch.

Local read-only diagnostic extension: `include/usbd.h`, `src/usbd_api.c`, and
`src/exports.tab` add ordinal 19, sceUsbdGetEnumerationInfo, and export minor
version 1.3. Existing API ordinals and the 1.2 ABI are retained. The extra
function reports enumeration state and the current descriptor request before
a device is ready, so the app can log failures that never reach its probe.

Only NETWORK=usb-ax88179 builds embed this module. Native SMAP builds keep
using the USB module supplied by the original pinned build image.

ABI 1.4 adds ordinal 20, `sceUsbdGetPipeInfo(pipeId, u32 output[8])`, as a
read-only snapshot for a bulk or interrupt endpoint. It runs under USBD's
lock, checks TD and request pointers against their allocation pools, and
does not expose pointers or payload bytes. Existing ordinals are retained.
Output words are:

0. Pipe ID.
1. OHCI endpoint flags (direction, packet size, USB address/endpoint).
2. Endpoint head low flags (bit 0 halted, bit 1 toggle); bit 8 indicates an
   active TD, bit 31 an invalid TD pointer detected without dereferencing it.
3. Active TD control/condition flags, or zero if no active TD.
4. Active request length, when its owner is valid.
5. Bytes progressed in that request (bounded by its length).
6. OHCI controller control flags.
7. Bit 0 means an unscheduled request is queued; bits 8+ hold USBD's internal
   TD-allocation queue state.

The hardware can advance during a snapshot; the lock prevents software
descriptor recycling. These are diagnostic observations, not a transaction
log or a promise that all fields represent the same instant.
