IOP_BIN = ax88179.irx
IOP_OBJS = driver.o frames.o imports.o
IOP_INCS += -I../../src
IOP_CFLAGS += -Wextra -Werror

all: $(IOP_BIN)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal

$(IOP_OBJS_DIR)driver.o: frames.h ../../src/ax88179_status.h
$(IOP_OBJS_DIR)frames.o: frames.h

clean:
	rm -f $(IOP_OBJS) $(IOP_BIN) *.notiopmod*.elf
	rm -rf obj
