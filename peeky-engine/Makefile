CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11 -I.
LDLIBS = -lm

SRCS = cssom.c layout.c paint.c render-tree.c tokenize.c tree.c
PROGS = $(SRCS:.c=)

all: $(PROGS)

%: %.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

layout: layout.h
paint: stb_image.h

clean:
	rm -f $(PROGS)

.PHONY: all clean
