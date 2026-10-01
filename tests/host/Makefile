# Host tests: plain C and Python checks of the firmware's pure logic, built
# with the host compiler (no Pico SDK) and run in seconds.
#
#   make -C tests/host test         # build and run everything; fails on any failure
#   make -C tests/host known-bugs   # the defects the firmware still has
#   make -C tests/host clean
#
# Every test_*.c here is one program: it includes the firmware units it tests
# from rp/src, with shim/ standing in for the few SDK headers they pull in, and
# exits non-zero when a check fails. Every test_*.py is a unittest module.
# Address and undefined-behaviour sanitizers make a write past a buffer fail
# the test instead of passing by luck.
#
# A bug_*.c test shows a defect the firmware still has: it fails today, and
# `known-bugs` passes only while every one of them does. When the defect is
# fixed the test starts passing: rename it to test_*.c so it guards the fix.

CC ?= cc
PYTHON ?= python3
REPO := ../..
BUILD := build

# The firmware's headers are included as system headers: the tests' own code
# is held to -Werror, the firmware's style is clang-tidy's job, and its
# headers do not all build warning-free with GCC yet.
CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -Wno-unknown-pragmas \
          -fsanitize=address,undefined -fno-omit-frame-pointer \
          -fno-sanitize-recover=undefined \
          -I shim -isystem $(REPO)/rp/src/include
LDFLAGS := -fsanitize=address,undefined -pthread

C_TESTS := $(patsubst %.c,$(BUILD)/%,$(wildcard test_*.c))
BUG_TESTS := $(patsubst %.c,$(BUILD)/%,$(wildcard bug_*.c))

.PHONY: test c-tests py-tests known-bugs clean FORCE

test: c-tests py-tests

c-tests: $(C_TESTS)
	@set -e; for t in $(C_TESTS); do echo "== $$t"; ./$$t; done

known-bugs: $(BUG_TESTS)
	@set -e; for t in $(BUG_TESTS); do echo "== $$t (must fail)"; \
	  if ./$$t > $$t.log 2>&1; then echo "$$t passes: the defect is fixed, rename it to test_*"; exit 1; \
	  else tail -3 $$t.log; fi; done

py-tests:
	@$(if $(wildcard test_*.py),$(PYTHON) -m unittest discover -s . -p 'test_*.py',echo "no Python tests")

# A test's own firmware sources are listed in its first line:
#   // units: rp/src/settings/settings.c
# Always rebuilt (it takes about a second): a test must never pass on a binary
# older than the firmware source it checks.
$(BUILD)/%: %.c FORCE | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< \
	  $(addprefix $(REPO)/,$(shell sed -n '1s|^// units: ||p' $<)) $(LDFLAGS)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

FORCE:
