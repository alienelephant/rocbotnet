# roclink-gw: the gateway daemon. ROCLINK_SRC is roclink's src/ (roclink-rfc).
ROCLINK_SRC ?= ../../../protocol/roclink-rfc/src
PREFIX ?= /usr/local

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -I$(ROCLINK_SRC) -DROCLINK_CRYPTO_OPENSSL
LDLIBS = -lmosquitto -lcjson -lcrypto -lpthread -lm

SRCS = roclink_gw.c $(ROCLINK_SRC)/roclink.c $(ROCLINK_SRC)/roclink_secure.c $(ROCLINK_SRC)/roclink_aes.c

roclink-gw: $(SRCS) $(ROCLINK_SRC)/roclink.h $(ROCLINK_SRC)/roclink_secure.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

install: roclink-gw
	install -D -m 0755 roclink-gw $(DESTDIR)$(PREFIX)/bin/roclink-gw

clean:
	rm -f roclink-gw

.PHONY: install clean
