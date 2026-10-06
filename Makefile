PKGS    := gtk4 libadwaita-1 libgpod-1.0 taglib_c gio-2.0
CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

PREFIX  ?= /usr/local
BIN     := ipod-uploader
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm644 data/ipod-uploader.desktop $(DESTDIR)$(PREFIX)/share/applications/ipod-uploader.desktop

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all install clean
