CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -std=c99 -O2
INCDIR   = include
SRCDIR   = src
BUILDDIR = build

SRCS = $(SRCDIR)/block.c $(SRCDIR)/crc32.c $(SRCDIR)/directory.c \
       $(SRCDIR)/ecc.c $(SRCDIR)/tapefs.c
OBJS = $(SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
TARGET = libtapefs.a

# ECC compiled at -O1 — -O2 aggressively DCE's the encoder body
$(BUILDDIR)/ecc.o: CFLAGS_EXTRA = -O1

.PHONY: all clean test

all: $(BUILDDIR)/$(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(CFLAGS_EXTRA) -I$(INCDIR) -c $< -o $@

$(BUILDDIR)/$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

$(BUILDDIR)/test_tapefs: $(OBJS)
	$(CC) $(CFLAGS) -I$(INCDIR) -o $@ $(OBJS) $(SRCDIR)/test_tapefs.c -lm

test: $(BUILDDIR)/test_tapefs
	./$(BUILDDIR)/test_tapefs

clean:
	rm -rf $(BUILDDIR)
