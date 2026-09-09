CC       ?= cc
CFLAGS   ?= -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Wshadow \
            -Wstrict-prototypes -g -O1 -Isrc
LDFLAGS  ?= -pthread

SRC   := $(wildcard src/*.c)
OBJ   := $(SRC:.c=.o)
TSRC  := $(wildcard test/test_*.c)
TBIN  := $(patsubst test/%.c,build/%,$(TSRC))

.PHONY: all test memcheck tsan clean

all: $(TBIN)

build:
	@mkdir -p build

build/%: test/%.c $(OBJ) test/harness.h | build
	$(CC) $(CFLAGS) $< $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Run every test binary; keep going after a failure so one broken module does
# not hide the state of the other ten, then fail the target if any failed.
test: $(TBIN)
	@rc=0; for t in $(TBIN); do \
	    echo "=== $$t ==="; \
	    $$t || rc=1; \
	done; exit $$rc

# --trace-children=yes is required, not optional: every test group is a fork,
# and without it valgrind checks the parent and nothing else.
memcheck: $(TBIN)
	@rc=0; for t in $(TBIN); do \
	    echo "=== valgrind $$t ==="; \
	    valgrind --trace-children=yes --error-exitcode=99 \
	             --leak-check=full --errors-for-leak-kinds=definite \
	             $$t || rc=1; \
	done; exit $$rc

# ThreadSanitizer, for days 6-11. Separate target because it is incompatible
# with valgrind and roughly 10x slower than a plain run.
tsan: CFLAGS += -fsanitize=thread -O1
tsan: LDFLAGS += -fsanitize=thread
tsan: clean $(TBIN)
	@rc=0; for t in $(TBIN); do \
	    echo "=== tsan $$t ==="; \
	    $$t || rc=1; \
	done; exit $$rc

clean:
	rm -f $(OBJ)
	rm -rf build
