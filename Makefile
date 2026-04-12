.RECIPEPREFIX := >

PLUGIN = wsyn_midi_customizer
BUNDLE = $(PLUGIN).lv2
CC = gcc
CFLAGS = -O3 -fPIC -Wall -Wextra -std=c11
LDFLAGS = -shared -lm
PKG = pkg-config
PKGFLAGS = $(shell $(PKG) --cflags --libs lv2)

all: $(BUNDLE)/$(PLUGIN).so

$(BUNDLE)/$(PLUGIN).so: $(PLUGIN).c manifest.ttl $(PLUGIN).ttl
> mkdir -p $(BUNDLE)
> $(CC) $(CFLAGS) $(PLUGIN).c -o $(BUNDLE)/$(PLUGIN).so $(PKGFLAGS) $(LDFLAGS)
> cp manifest.ttl $(PLUGIN).ttl $(BUNDLE)/

install: all
> mkdir -p ~/.lv2
> rm -rf ~/.lv2/$(BUNDLE)
> cp -r $(BUNDLE) ~/.lv2/

clean:
> rm -rf $(BUNDLE)