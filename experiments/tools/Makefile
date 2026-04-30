CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra

TARGETS := sender receiver

.PHONY: all clean

all: $(TARGETS)

sender: sender.c
	$(CC) $(CFLAGS) -o $@ $<

receiver: receiver.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)
