# roclink: build and test on a PC.
#   make test            all tests (security uses OpenSSL's AES)
#   make test-backends   the security tests again with the PSA Crypto provider
#   make test-jumbo      everything again with -D ROCLINK_JUMBO (1470-byte frames)
#   make sanitize        the tests under AddressSanitizer + UBSan
#   make cxx-check       the library compiles as C++ (for .ino / C++ projects)
#   make config-check    frame-size misconfigurations must fail to compile
#   make vectors         regenerate test/vectors_secure.h (needs python3-cryptography)

CC ?= cc
CXX ?= c++
WARN := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wstrict-prototypes -Werror
CFLAGS ?= -std=c99 -O2 $(WARN)
BUILD := build

CORE := src/roclink.c
SECURE := src/roclink_secure.c src/roclink_aes.c
OPENSSL := -DROCLINK_CRYPTO_OPENSSL
OPENSSL_LIBS := -lcrypto

TESTS := $(BUILD)/test_frame $(BUILD)/test_client $(BUILD)/test_server $(BUILD)/test_exchange \
         $(BUILD)/test_secure

.PHONY: all test test-backends test-jumbo sanitize cxx-check config-check vectors clean

all: test

$(BUILD):
	mkdir -p $@

$(BUILD)/test_%: test/test_%.c test/check.h src/roclink.h $(CORE) | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $< $(CORE) -o $@

$(BUILD)/test_secure: test/test_secure.c test/check.h test/vectors_secure.h src/roclink.h src/roclink_secure.h $(CORE) $(SECURE) | $(BUILD)
	$(CC) $(CFLAGS) $(OPENSSL) -Isrc $< $(CORE) $(SECURE) $(OPENSSL_LIBS) -o $@

test: $(TESTS)
	@set -e; for t in $(abspath $(TESTS)); do echo "== $$t"; $$t; done

# The PSA Crypto provider (what Zephyr uses), against TF-PSA-Crypto or mbedTLS.
PSA_CFLAGS ?=
PSA_LIBS ?= $(shell pkg-config --libs tfpsacrypto 2>/dev/null || pkg-config --libs mbedcrypto 2>/dev/null || echo -lmbedcrypto)
$(BUILD)/test_secure_psa: test/test_secure.c test/check.h test/vectors_secure.h src/roclink.h src/roclink_secure.h $(CORE) $(SECURE) | $(BUILD)
	$(CC) $(CFLAGS) -DROCLINK_CRYPTO_PSA $(PSA_CFLAGS) -Isrc $< $(CORE) $(SECURE) $(PSA_LIBS) -o $@

test-backends: $(BUILD)/test_secure $(BUILD)/test_secure_psa
	@set -e; for t in $(abspath $^); do echo "== $$t"; $$t; done

test-jumbo:
	@$(MAKE) --no-print-directory BUILD=$(BUILD)/jumbo CFLAGS="$(CFLAGS) -DROCLINK_JUMBO" test test-backends

sanitize: CFLAGS := -std=c99 -g -O1 $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all
sanitize: clean test

cxx-check: | $(BUILD)
	@for f in $(CORE) $(SECURE); do \
		echo "c++ $$f"; \
		$(CXX) -x c++ -std=c++11 -Wall -Wextra -Wpedantic -Werror $(OPENSSL) -Isrc -c $$f -o $(BUILD)/cxx.o || exit 1; \
	done

# Each "bad" configuration must be refused by the compiler; each "good" one must compile.
CFG_BAD := "-DROCLINK_FRAME_MAX_LEN=300" \
           "-DROCLINK_JUMBO -DROCLINK_FRAME_MAX_LEN=200" \
           "-DROCLINK_JUMBO -DROCLINK_FRAME_MAX_LEN=70000"
CFG_GOOD := "" "-DROCLINK_FRAME_MAX_LEN=255" "-DROCLINK_JUMBO" "-DROCLINK_JUMBO -DROCLINK_FRAME_MAX_LEN=4000"
config-check:
	@for f in $(CFG_BAD); do \
		if echo '#include "roclink.h"' | $(CC) -std=c99 -Isrc $$f -fsyntax-only -x c - 2>/dev/null; then \
			echo "FAIL: accepted $$f"; exit 1; else echo "ok: refused $$f"; fi; \
	done
	@for f in $(CFG_GOOD); do \
		echo '#include "roclink.h"' | $(CC) -std=c99 -Isrc $$f -fsyntax-only -x c - || { echo "FAIL: refused $$f"; exit 1; }; \
		echo "ok: accepted $${f:-(defaults)}"; \
	done

vectors:
	@{ echo "/* Secured test vectors for test_secure.c. Regenerate: python3 tools/gen_vectors.py */"; \
	   echo "#ifndef ROCLINK_TEST_VECTORS_SECURE_H"; echo "#define ROCLINK_TEST_VECTORS_SECURE_H"; \
	   echo "#include <stdint.h>"; python3 tools/gen_vectors.py | sed '/^$$/,$$d'; echo "#endif"; } > test/vectors_secure.h

clean:
	rm -rf $(BUILD)
