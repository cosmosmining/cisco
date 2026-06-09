# PacketForge — plain Makefile, no magic.
#
#   make                  build library + apps into build/
#   make test             build + run unit tests
#   make SAN=asan ...     AddressSanitizer+UBSan build (objects in build-asan/)
#   make SAN=ubsan ...    UBSan-only build
#   make COV=1 ...        gcov instrumentation
#   make check-format     clang-format dry run (fails on drift)
#   make tidy             clang-tidy over src/ and apps/
#   make clean

CC ?= gcc
SAN ?=
COV ?=

BUILD := build$(if $(SAN),-$(SAN),)$(if $(COV),-cov,)

WARN := -Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wstrict-prototypes \
        -Wmissing-prototypes -Wundef -Wvla
OPT ?= -O2
CFLAGS += -std=c11 -D_DEFAULT_SOURCE $(OPT) -g $(WARN) -MMD -MP -Isrc
LDFLAGS +=
LDLIBS +=

ifeq ($(SAN),asan)
  CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
  LDFLAGS += -fsanitize=address,undefined
  OPT := -O1
endif
ifeq ($(SAN),ubsan)
  CFLAGS += -fsanitize=undefined -fno-sanitize-recover=all
  LDFLAGS += -fsanitize=undefined
endif
ifeq ($(COV),1)
  CFLAGS += --coverage -O0
  LDFLAGS += --coverage
endif

LIB_SRCS := $(wildcard src/core/*.c src/eth/*.c src/arp/*.c src/ipv4/*.c \
              src/icmp/*.c src/udp/*.c src/tcp/*.c src/route/*.c src/fwd/*.c \
              src/cli/*.c src/netdev/*.c)
LIB_OBJS := $(LIB_SRCS:%.c=$(BUILD)/%.o)
LIB      := $(BUILD)/libpacketforge.a

APP_SRCS := $(wildcard apps/*.c)
APPS     := $(APP_SRCS:apps/%.c=$(BUILD)/bin/%)

TEST_SRCS := $(wildcard test/unit/test_*.c)
TESTS     := $(TEST_SRCS:test/unit/%.c=$(BUILD)/test/%)

.PHONY: all test clean check-format tidy

all: $(LIB) $(APPS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

APP_HDRS := $(wildcard apps/*.h)

$(BUILD)/bin/%: apps/%.c $(APP_HDRS) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

# Unit tests: each test_*.c is its own binary. Tests may define pf_now_ms()
# to inject a fake clock; the archive member providing the real one is then
# simply never pulled in.
TEST_HDRS := $(wildcard test/unit/*.h)

$(BUILD)/test/%: test/unit/%.c $(TEST_HDRS) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itest/unit $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

test: $(TESTS)
	@rc=0; for t in $(TESTS); do \
	  printf '== %s\n' $$t; ./$$t || rc=1; \
	done; exit $$rc

# Fuzz harness (also a corpus replayer when built with a normal compiler).
$(BUILD)/fuzz/parse_harness: fuzz/parse_harness.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

fuzz-replay: $(BUILD)/fuzz/parse_harness
	@n=0; for f in fuzz/corpus/*.bin; do ./$< < $$f || exit 1; n=$$((n+1)); done; \
	echo "replayed $$n corpus seeds, no crashes"

FORMAT_SRCS := $(shell find src apps test/unit fuzz -name '*.[ch]' 2>/dev/null)

check-format:
	clang-format --dry-run -Werror $(FORMAT_SRCS)

format:
	clang-format -i $(FORMAT_SRCS)

TIDY_SRCS := $(LIB_SRCS) $(APP_SRCS)

tidy:
	clang-tidy --quiet $(TIDY_SRCS) -- $(CFLAGS)

clean:
	rm -rf build build-*

-include $(LIB_OBJS:.o=.d)
