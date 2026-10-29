# SPDX-License-Identifier: GPL-2.0-only
CFLAGS ?= -O2 -Wall
CPPFLAGS += -I../include
LDLIBS += -lfdt

.PHONY: all clean test

all: quest-el1

# The /init of the target's first initramfs: static, no libfdt, no shell.
handoff-init: handoff-init.c
	$(CC) $(CFLAGS) -static -o $@ handoff-init.c

quest-el1: quest-el1.c ../include/quest_el1_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ quest-el1.c $(LDFLAGS) $(LDLIBS)

planner-test: quest-el1.c planner_test.c ../include/quest_el1_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-function -DQUEST_EL1_TEST -o $@ planner_test.c $(LDFLAGS) $(LDLIBS)

test: planner-test
	./planner-test

clean:
	rm -f redir.so quest-el1 planner-test handoff-init
