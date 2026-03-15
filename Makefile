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
# 'make install-icons' installs the official Fluxer icons system-wide (requires sudo).
ICON_DIR = /usr/share/pixmaps/pidgin/protocols

.PHONY: all clean install install-user uninstall uninstall-user install-icons uninstall-icons

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

uninstall:
	rm -f $(SYSTEM_PLUGINDIR)/$(PLUGIN_SO)

uninstall-user:
	rm -f $(USER_PLUGINDIR)/$(PLUGIN_SO)

# Pidgin 2.x has no user-local icon path — system install is unavoidable.
install-icons:
	sudo install -Dm644 icons/fluxer16.png $(ICON_DIR)/16/fluxer.png
	sudo install -Dm644 icons/fluxer22.png $(ICON_DIR)/22/fluxer.png
	sudo install -Dm644 icons/fluxer48.png $(ICON_DIR)/48/fluxer.png

uninstall-icons:
	sudo rm -f $(ICON_DIR)/16/fluxer.png $(ICON_DIR)/22/fluxer.png \
	           $(ICON_DIR)/48/fluxer.png

clean:
	rm -f $(OBJS) $(PLUGIN_SO)
