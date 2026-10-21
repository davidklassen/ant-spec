# antc build (ROADMAP.md): `make runtime` builds the C runtime archive that `antc build` links
# and writes the flags it links with next to it; `make stage1` builds antc under the bootstrap,
# `make stage2` rebuilds it with stage1, `make fixpoint` checks that stage2 and stage3 emit the
# same C for antc's own source and installs stage2 as bin/antc. GC_PREFIX is where Homebrew put bdw-gc.
ifeq ($(origin CC),default)
CC = clang
endif
GC_PREFIX ?= /opt/homebrew
PYTHON ?= python3
CFLAGS = -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I$(GC_PREFIX)/include
RT_LDFLAGS = -L$(GC_PREFIX)/lib -lgc -lpthread

RT_SRC = $(wildcard runtime/*.c)
RT_OBJ = $(patsubst runtime/%.c,runtime/obj/%.o,$(RT_SRC))
ANTC_SRC = $(wildcard src/antc/*.ant src/antc/*/*.ant)
BOOTSTRAP_SRC = bootstrap/antpy.py $(wildcard bootstrap/antpy/*.py) tools/antparse.py
# One spelling for the entry: the emitted C embeds module paths as typed, and the fixpoint compares bytes.
ANTC_ENTRY = src/antc/main.ant
export ANT_CC = $(CC)

.PHONY: runtime stage1 stage2 fixpoint clean FORCE
# A recipe that fails leaves no half-written target behind.
.DELETE_ON_ERROR:

runtime: runtime/libantrt.a runtime/libantrt.flags

runtime/libantrt.a: $(RT_OBJ)
	ar rcs $@ $^

runtime/obj/%.o: runtime/%.c runtime/ant.h | runtime/obj
	$(CC) $(CFLAGS) -c $< -o $@

runtime/obj:
	mkdir -p $@

# Rewritten only when its content changes: a changed GC_PREFIX reaches `antc build`,
# and an unchanged one does not rebuild the stages.
runtime/libantrt.flags: FORCE
	@echo "$(RT_LDFLAGS)" | cmp -s - $@ 2>/dev/null || echo "$(RT_LDFLAGS)" > $@

FORCE:

stage1: bin/antc-stage1
stage2: bin/antc-stage2
fixpoint: bin/antc

bin:
	mkdir -p $@

# Stage 1: antc under the bootstrap interpreter compiles antc.
bin/antc-stage1: $(ANTC_SRC) $(BOOTSTRAP_SRC) runtime/libantrt.a runtime/libantrt.flags | bin
	$(PYTHON) -I bootstrap/antpy.py run $(ANTC_ENTRY) build $(ANTC_ENTRY) -o $@

# Stage 2: the native stage1 compiles antc; stage 3: stage2 does. Each stage depends on the one
# that builds it, and stage1 already depends on every source, so a changed source rebuilds the chain.
# Safe under make -j: `--emit-c` writes only its output, and `antc build` names its temporary C and
# binary by the clocks and the entry (build.ant tempPath), never by the output path.
bin/antc-stage2: bin/antc-stage1
	$< build $(ANTC_ENTRY) -o $@

bin/antc-stage3: bin/antc-stage2
	$< build $(ANTC_ENTRY) -o $@

# bin/<stage>.c: the C that stage emits for antc's own source; the fixpoint compares stage2's and stage3's.
bin/%.c: bin/%
	$< build --emit-c $(ANTC_ENTRY) -o $@

# bin/antc is stage2, installed only once the fixpoint holds; a stale copy never survives a mismatch.
bin/antc: bin/antc-stage2 bin/antc-stage2.c bin/antc-stage3.c
	rm -f $@
	cmp bin/antc-stage2.c bin/antc-stage3.c
	cp bin/antc-stage2 $@

clean:
	rm -rf runtime/obj runtime/libantrt.a runtime/libantrt.flags bin
