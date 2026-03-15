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

.PHONY: all clean install install-user

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

clean:
	rm -f $(OBJS) $(PLUGIN_SO)
