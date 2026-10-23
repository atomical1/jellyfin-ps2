# Build the vendored PS2SDK USBD with the app's pinned toolchain.
IOP_BIN = usbd.irx
IOP_SRC_DIR = src/
IOP_INCS += -Isrc -Iinclude
IOP_OBJS = hub.o mem.o timer.o endpoint.o io_request.o hub_resets.o \
           td_queue.o hcd.o usbd_sys.o usbd_api.o usbd_main.o \
           report_descriptor_init.o device_driver.o device.o imports.o exports.o
all: $(IOP_BIN)
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
$(IOP_OBJS): src/usbdpriv.h src/irx_imports.h include/usbd.h
.PHONY: clean
clean:
	rm -rf obj
	rm -f usbd.irx *.notiopmod*.elf
