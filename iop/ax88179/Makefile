IOP_BIN = ax88179.irx
IOP_OBJS = driver.o frames.o imports.o
IOP_INCS += -I../../src -I../../third_party/ps2sdk-usbd/include
IOP_CFLAGS += -Wextra -Werror

all: $(IOP_BIN)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal

$(IOP_OBJS_DIR)driver.o: frames.h ../../src/ax88179_status.h
$(IOP_OBJS_DIR)driver.o $(IOP_OBJS_DIR)imports.o: ../../third_party/ps2sdk-usbd/include/usbd.h
$(IOP_OBJS_DIR)frames.o: frames.h

clean:
	rm -f $(IOP_OBJS) $(IOP_BIN) *.notiopmod*.elf
	rm -rf obj
