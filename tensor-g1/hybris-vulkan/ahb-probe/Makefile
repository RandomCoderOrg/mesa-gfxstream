CC ?= cc
PREFIX ?= /root/hybris-rootless/ahb-probe-prefix
HYBRIS_BUILD ?= /root/hybris-rootless/src/libhybris-upstream/hybris
ANDROID_INCLUDE ?= /root/hybris-rootless/prefix/include

CPPFLAGS += -I$(ANDROID_INCLUDE) -I$(HYBRIS_BUILD)/include
CFLAGS += -O2 -g -Wall -Wextra -Werror -fPIC
HYBRIS_LIBDIR := $(HYBRIS_BUILD)/common/.libs

.PHONY: all clean host-test install

all: libahb-wrapper.so ahb-probe

libahb-wrapper.so: ahb-wrapper.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared -o $@ $< \
		-L$(HYBRIS_LIBDIR) -Wl,-rpath,$(HYBRIS_LIBDIR) -lhybris-common -pthread

ahb-probe: ahb-probe.c libahb-wrapper.so
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ ahb-probe.c \
		-L. -Wl,-rpath,'$$ORIGIN' -lahb-wrapper

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib
	install -m 0755 ahb-probe $(DESTDIR)$(PREFIX)/bin/
	install -m 0755 libahb-wrapper.so $(DESTDIR)$(PREFIX)/lib/

host-test:
	$(CC) -std=c11 -O2 -g -Wall -Wextra -Werror -Itest/mock \
		ahb-probe.c test/mock/mock-ahb.c -o /tmp/udroid-ahb-probe-host
	/tmp/udroid-ahb-probe-host 2 8 | \
		jq -se 'length == 18 and all(.result == 0 and .expected_hash == .actual_hash and .stride >= .width)'

clean:
	$(RM) ahb-probe libahb-wrapper.so
