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
