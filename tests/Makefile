CC ?= cc
CFLAGS ?= -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer
TEST_BIN = ../build/ax88179-frames-test

.PHONY: all clean
all: $(TEST_BIN)
	$(TEST_BIN)
	python3 usbd_enumeration_test.py
	python3 ax88179_link_test.py
	python3 dhcp_wait_test.py
	python3 ax88179_notification_test.py
	python3 usbd_pipe_test.py

$(TEST_BIN): ax88179_frames_test.c ../iop/ax88179/frames.c ../iop/ax88179/frames.h
	mkdir -p ../build
	$(CC) $(CFLAGS) ax88179_frames_test.c ../iop/ax88179/frames.c -o $@

clean:
	rm -f $(TEST_BIN)
