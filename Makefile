CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -Iinclude -pthread 
DBGFLAGS:= -g
OPTFLAGS:= -O2
ASAN    := -fsanitize=address,undefined

SRC := src/my-malloc.c
TEST_DIR := test

BENCH_SRC := $(TEST_DIR)/benchmark.c
BENCH_BIN := $(TEST_DIR)/benchmark

TEST_SRCS := $(filter-out $(BENCH_SRC),$(wildcard $(TEST_DIR)/*.c))
TEST_BINS := $(TEST_SRCS:.c=)

BENCH_OPS ?= 5000
TARGET ?= $(firstword $(TEST_BINS))

.PHONY: all test asan bench run clean valgrind gdb help threads s thread bug

all: $(TEST_BINS) $(BENCH_BIN)

help:
	@echo "Targets:"
	@echo "  make            - build every test binary and the benchmark"
	@echo "  make test       - build and run all test/*.c binaries (excludes benchmark)"
	@echo "  make asan       - rebuild everything with -fsanitize=address,undefined and run it"
	@echo "  make bench      - build and run the benchmark (BENCH_OPS=$(BENCH_OPS) by default)"
	@echo "  make threads    - build and run just test/test_threads (the pthread stress test)"
	@echo "  make run        - alias for 'make test'"
	@echo "  make valgrind   - run TARGET under valgrind (default: $(TARGET))"
	@echo "  make gdb        - open TARGET in gdb (default: $(TARGET))"
	@echo "  make clean      - remove all built binaries"
	@echo "Current test binaries: $(TEST_BINS)"

$(TEST_DIR)/%: $(TEST_DIR)/%.c $(SRC) include/my-malloc.h include/list.h
	$(CC) $(CFLAGS) $(DBGFLAGS) -o $@ $(SRC) $<

$(BENCH_BIN): $(BENCH_SRC) $(SRC) include/my-malloc.h include/list.h
	$(CC) $(CFLAGS) $(OPTFLAGS) $(DBGFLAGS) -o $@ $(SRC) $(BENCH_SRC)

test: $(TEST_BINS)
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "== running $$t =="; \
		./$$t || status=1; \
	done; \
	exit $$status

run: test

bench: $(BENCH_BIN)
	./$(BENCH_BIN) --ops $(BENCH_OPS)

threads: $(TEST_DIR)/test_threads
	@echo "== running $(TEST_DIR)/test_threads =="
	./$(TEST_DIR)/test_threads

asan: CFLAGS += $(ASAN)
asan: clean $(TEST_BINS) $(BENCH_BIN)
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "== running $$t (asan) =="; \
		./$$t || status=1; \
	done; \
	echo "== running $(BENCH_BIN) (asan, BENCH_OPS=$(BENCH_OPS)) =="; \
	./$(BENCH_BIN) --ops $(BENCH_OPS) --no-libc || status=1; \
	exit $$status

valgrind: $(TARGET)
	valgrind --error-exitcode=1 --leak-check=full ./$(TARGET)

gdb: $(TARGET)
	gdb ./$(TARGET)

s:
	$(CC) $(CFLAGS) $(OPTFLAGS) $(DBGFLAGS) -o test/benchmark $(SRC) test/benchmark.c
	./test/benchmark --ops 5000 --seed 42

thread:
	$(CC) $(CFLAGS) $(DBGFLAGS) -O0 -o test/test_threads_helgrind $(SRC) test/test_threads.c
	valgrind --tool=helgrind --history-level=full ./test/test_threads_helgrind

bug:
	$(CC) $(CFLAGS) -DDEBUG $(ASAN) $(DBGFLAGS) -Isrc -o test/test_bugs test/test_bugs.c $(SRC) src/debug.c
	./test/test_bugs

clean:
	rm -f $(TEST_BINS) $(BENCH_BIN) test/test_threads_helgrind