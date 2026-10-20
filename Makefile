# antc build (ROADMAP.md). `make runtime` builds the C runtime archive that
# `antc build` links and writes the flags it links with next to it; the
# stage targets arrive with step 2.5. GC_PREFIX is where Homebrew put bdw-gc.
ifeq ($(origin CC),default)
CC = clang
endif
GC_PREFIX ?= /opt/homebrew
CFLAGS = -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I$(GC_PREFIX)/include
RT_LDFLAGS = -L$(GC_PREFIX)/lib -lgc -lpthread

RT_SRC = $(wildcard runtime/*.c)
RT_OBJ = $(patsubst runtime/%.c,runtime/obj/%.o,$(RT_SRC))

.PHONY: runtime clean

# The flags file is rewritten on every run, so a changed GC_PREFIX reaches `antc build`.
runtime: runtime/libantrt.a
	@echo "$(RT_LDFLAGS)" > runtime/libantrt.flags

runtime/libantrt.a: $(RT_OBJ)
	ar rcs $@ $^

runtime/obj/%.o: runtime/%.c runtime/ant.h | runtime/obj
	$(CC) $(CFLAGS) -c $< -o $@

runtime/obj:
	mkdir -p $@

clean:
	rm -rf runtime/obj runtime/libantrt.a runtime/libantrt.flags
