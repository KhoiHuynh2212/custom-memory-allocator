CC       := gcc
CFLAGS   := -Wall -Wextra -std=c11 -Iinclude -pthread
DBGFLAGS := -g
OPTFLAGS := -O2
ASAN     := -fsanitize=address,undefined

SRC       := src/my-malloc.c
DEBUG_SRC := src/debug.c
TEST_DIR  := test

# Test files that #include debug.h and call its check_*()/debug_get_state()
# functions need -DDEBUG (or those calls compile away as no-ops) and need
# src/debug.c linked in. List their basenames here — nothing else to touch.
# A test that only includes debug.h for constants (HEADER_SIZE, align, ...)
# but never calls a check_*() function does NOT need to be listed.
DEBUG_TESTS := test_bugs test-edge-cases

BENCH_SRC := $(TEST_DIR)/benchmark.c
BENCH_BIN := $(TEST_DIR)/benchmark

# Every test/*.c except benchmark.c becomes one buildable, runnable test.
ALL_TEST_SRCS  := $(filter-out $(BENCH_SRC),$(wildcard $(TEST_DIR)/*.c))
ALL_TEST_NAMES := $(basename $(notdir $(ALL_TEST_SRCS)))
ALL_TEST_BINS  := $(addprefix $(TEST_DIR)/,$(ALL_TEST_NAMES))

BENCH_OPS ?= 5000
TARGET    ?= $(firstword $(ALL_TEST_NAMES))

.PHONY: all help list-tests test run asan bench valgrind gdb clean $(ALL_TEST_NAMES)

all: $(ALL_TEST_BINS) $(BENCH_BIN)

help:
	@echo "Run ONE test (build + run, single command):"
	@for t in $(ALL_TEST_NAMES); do echo "  make $$t"; done
	@echo ""
	@echo "Other targets:"
	@echo "  make list-tests            - print all discovered test names"
	@echo "  make test                  - build and run every test/*.c"
	@echo "  make asan                  - rebuild everything with ASan+UBSan and run it"
	@echo "  make bench                 - run the benchmark (BENCH_OPS=$(BENCH_OPS))"
	@echo "  make valgrind TARGET=name  - run one test under valgrind (default: $(TARGET))"
	@echo "  make gdb TARGET=name       - open one test in gdb (default: $(TARGET))"
	@echo "  make clean                 - remove all built binaries"

list-tests:
	@echo $(ALL_TEST_NAMES)

# --- default build rule: any test/foo.c -> test/foo, plain build ---
$(TEST_DIR)/%: $(TEST_DIR)/%.c $(SRC) include/my-malloc.h src/list.h
	$(CC) $(CFLAGS) $(DBGFLAGS) -o $@ $(SRC) $<

# --- override rule for each name in DEBUG_TESTS: adds -DDEBUG, ASan, debug.c ---
define DEBUG_TEST_RULE
$(TEST_DIR)/$(1): $(TEST_DIR)/$(1).c $(SRC) $(DEBUG_SRC) src/debug.h include/my-malloc.h src/list.h
	$(CC) $(CFLAGS) -DDEBUG $(ASAN) $(DBGFLAGS) -o $$@ $(SRC) $(DEBUG_SRC) $$<
endef
$(foreach t,$(DEBUG_TESTS),$(eval $(call DEBUG_TEST_RULE,$(t))))

# --- one phony shortcut per discovered test file: `make test_basic` etc ---
define TEST_SHORTCUT
$(1): $(TEST_DIR)/$(1)
	@echo "== running $(TEST_DIR)/$(1) =="
	@./$(TEST_DIR)/$(1)
endef
$(foreach t,$(ALL_TEST_NAMES),$(eval $(call TEST_SHORTCUT,$(t))))

test: $(ALL_TEST_BINS)
	@status=0; \
	for t in $(ALL_TEST_BINS); do \
		echo "== running $$t =="; \
		./$$t || status=1; \
	done; \
	exit $$status

run: test

$(BENCH_BIN): $(BENCH_SRC) $(SRC) include/my-malloc.h src/list.h
	$(CC) $(CFLAGS) $(OPTFLAGS) $(DBGFLAGS) -o $@ $(SRC) $(BENCH_SRC)

bench: $(BENCH_BIN)
	./$(BENCH_BIN) --ops $(BENCH_OPS)

asan: CFLAGS += $(ASAN)
asan: clean $(ALL_TEST_BINS) $(BENCH_BIN)
	@status=0; \
	for t in $(ALL_TEST_BINS); do \
		echo "== running $$t (asan) =="; \
		./$$t || status=1; \
	done; \
	echo "== running $(BENCH_BIN) (asan, BENCH_OPS=$(BENCH_OPS)) =="; \
	./$(BENCH_BIN) --ops $(BENCH_OPS) --no-libc || status=1; \
	exit $$status

valgrind: $(TEST_DIR)/$(TARGET)
	valgrind --error-exitcode=1 --leak-check=full ./$(TEST_DIR)/$(TARGET)

gdb: $(TEST_DIR)/$(TARGET)
	gdb ./$(TEST_DIR)/$(TARGET)

clean:
	rm -f $(ALL_TEST_BINS) $(BENCH_BIN)