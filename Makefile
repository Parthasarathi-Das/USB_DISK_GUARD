CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wno-format-truncation -O2
SRC      = main.c util.c disk.c guard.c prompt.c

usbguard: $(SRC) config.h util.h disk.h guard.h prompt.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -f usbguard
