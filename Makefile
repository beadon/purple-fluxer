# Makefile for purple-fluxer
# Build deps: libpurple-dev, libjson-glib-dev, libglib2.0-dev

PLUGIN_ID    = purple-fluxer
PLUGIN_SO    = $(PLUGIN_ID).so

CC           = gcc
CFLAGS       = -O2 -Wall -Wextra -g -fPIC \
               $(shell pkg-config --cflags purple json-glib-1.0 glib-2.0)
LDSOFLAGS    = -shared
LDLIBS       = $(shell pkg-config --libs purple json-glib-1.0 glib-2.0)

SRCS         = purple-fluxer.c
OBJS         = $(SRCS:.c=.o)

# Install dirs
SYSTEM_PLUGINDIR = $(shell pkg-config --variable=plugindir purple)
USER_PLUGINDIR   = $(HOME)/.purple/plugins

# Pidgin 2.x hardcodes protocol icons to the system DATADIR — no user override path exists.
# 'make install-icons' installs placeholder icons system-wide (requires sudo).
ICON_SIZES       = 16 22 48
ICON_DIR         = /usr/share/pixmaps/pidgin/protocols

.PHONY: all clean install install-user install-icons icons/fluxer.png

all: $(PLUGIN_SO)

$(PLUGIN_SO): $(OBJS)
	$(CC) $(LDSOFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(PLUGIN_SO)
	install -d $(SYSTEM_PLUGINDIR)
	install -m 644 $(PLUGIN_SO) $(SYSTEM_PLUGINDIR)

install-user: $(PLUGIN_SO)
	install -d $(USER_PLUGINDIR)
	install -m 644 $(PLUGIN_SO) $(USER_PLUGINDIR)

# Pidgin 2.x has no user-local icon path — system install is unavoidable.
# Generate a minimal placeholder PNG at each required size and install it.
install-icons: icons/fluxer.png
	$(foreach sz,$(ICON_SIZES), \
	  sudo install -Dm644 icons/fluxer.png $(ICON_DIR)/$(sz)/fluxer.png ;)

icons/fluxer.png:
	mkdir -p icons
	python3 -c "\
import struct, zlib; \
def png(w,h,rgb): \
  r,g,b=rgb; raw=b''.join(b'\x00'+bytes([r,g,b]*w) for _ in range(h)); \
  idat=zlib.compress(raw); \
  def chunk(t,d): c=zlib.crc32(t+d)&0xffffffff; return struct.pack('>I',len(d))+t+d+struct.pack('>I',c); \
  return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',idat)+chunk(b'IEND',b''); \
open('icons/fluxer.png','wb').write(png(48,48,(0x5c,0x6b,0xff)))"

clean:
	rm -f $(OBJS) $(PLUGIN_SO)
