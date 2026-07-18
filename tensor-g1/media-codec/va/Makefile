CC ?= cc
PREFIX ?= /opt/tensor-va
DRIVER_DIR ?= $(PREFIX)/lib/dri
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -fPIC -Wall -Wextra
LDFLAGS += -shared -Wl,-soname,tensor_drv_video.so

all: tensor_drv_video.so

tensor_drv_video.so: tensor_drv_video.c ../bridge-protocol.h
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

install: tensor_drv_video.so
	install -Dm755 tensor_drv_video.so $(DESTDIR)$(DRIVER_DIR)/tensor_drv_video.so

clean:
	rm -f tensor_drv_video.so

.PHONY: all clean install
