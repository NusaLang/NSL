CXX ?= g++
CC ?= cc
CXXFLAGS := -std=c++17 -O3 -flto=auto -DNDEBUG -Wall -Wextra -Iinclude -Ithird_party/quickjs/vendor -Ithird_party/bearssl/vendor/inc -Ithird_party/stb/vendor -Ithird_party/quirc/vendor -pthread
SRC := $(filter-out src/wasm_main.cpp, $(wildcard src/*.cpp))
BIN := nusantara
PLUGIN_DIRS := $(wildcard plugins/*)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	SHLIB_FLAG := -dynamiclib
	SHLIB_EXT := dylib
else
	SHLIB_FLAG := -shared
	SHLIB_EXT := so
endif

# --- QuickJS (vendored, single-source-of-truth for .js execution) ----------
# Amalgamation-style vendoring, same convention as plugins/sqlite/vendor's
# sqlite3.c: source dropped in verbatim under third_party/quickjs/vendor,
# compiled once as plain C objects and cached (quickjs.c alone is ~55k
# lines -- recompiling it with every `make` would make iteration painful).
# Linked straight into the main $(BIN)/$(OPT_BIN), not built as a plugin
# .so, since .js execution is a core dispatch branch in main.cpp, not a
# muat_plugin()-loadable library.
QJS_DIR := third_party/quickjs/vendor
QJS_VERSION := $(shell cat $(QJS_DIR)/VERSION)
QJS_SRCS := $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c $(QJS_DIR)/libunicode.c $(QJS_DIR)/libbf.c $(QJS_DIR)/cutils.c
QJS_OBJS := $(QJS_SRCS:.c=.o)
QJS_CFLAGS := -O2 -w -D_GNU_SOURCE -DCONFIG_VERSION=\"$(QJS_VERSION)\" -DCONFIG_BIGNUM

$(QJS_DIR)/%.o: $(QJS_DIR)/%.c
	$(CC) $(QJS_CFLAGS) -c $< -o $@

# --- BearSSL (vendored, TLS for net::httpRequest's https:// path) ----------
# Same vendoring convention as QuickJS above: upstream source dropped in
# verbatim under third_party/bearssl/vendor/src, compiled once and cached.
# BR_AES_X86NI/BR_SSE2/BR_POWER8 disabled so the portable constant-time
# implementations are always the ones actually linked in, regardless of
# which CPU this happens to be built on -- this same object set has to be
# correct on x86-64, ARM64 and (cross-compiled) Windows alike.
BSSL_DIR := third_party/bearssl/vendor/src
BSSL_SRCS := $(wildcard $(BSSL_DIR)/*.c)
BSSL_OBJS := $(BSSL_SRCS:.c=.o)
BSSL_CFLAGS := -O2 -w -Ithird_party/bearssl/vendor/inc -I$(BSSL_DIR) -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0

$(BSSL_DIR)/%.o: $(BSSL_DIR)/%.c
	$(CC) $(BSSL_CFLAGS) -c $< -o $@

# quirc (vendored, QR decode for qr_baca()). stb_image is header-only,
# compiled via src/qr.cpp, no separate object rule needed.
QUIRC_DIR := third_party/quirc/vendor
QUIRC_SRCS := $(wildcard $(QUIRC_DIR)/*.c)
QUIRC_OBJS := $(QUIRC_SRCS:.c=.o)
QUIRC_CFLAGS := -O2 -w -I$(QUIRC_DIR)

$(QUIRC_DIR)/%.o: $(QUIRC_DIR)/%.c
	$(CC) $(QUIRC_CFLAGS) -c $< -o $@

.PHONY: all clean run plugins clean-plugins test test-update opt sizeof bench

all: $(BIN)

$(BIN): $(SRC) $(QJS_OBJS) $(BSSL_OBJS) $(QUIRC_OBJS)
	$(CXX) $(CXXFLAGS) $(SRC) $(QJS_OBJS) $(BSSL_OBJS) $(QUIRC_OBJS) -o $(BIN) -ldl -lm

run: $(BIN)
	./$(BIN) run examples/hello.ns

# --- build terpisah buat kerjaan optimasi -----------------------------------
# JANGAN PERNAH nimpa ~/.local/bin/nusa: itu dipakai service produksi yang
# lagi jalan (bot WhatsApp, backend, registry). Semua eksperimen dibangun
# ke build-opt/nusa, dan test runner default-nya nunjuk ke situ.
OPT_BIN := build-opt/nusa

opt: $(OPT_BIN)

$(OPT_BIN): $(SRC) $(wildcard include/*.hpp) $(QJS_OBJS) $(BSSL_OBJS) $(QUIRC_OBJS)
	@mkdir -p build-opt
	@ln -sfn ../nusantara-plugins build-opt/nusantara-plugins
	$(CXX) $(CXXFLAGS) $(SRC) $(QJS_OBJS) $(BSSL_OBJS) $(QUIRC_OBJS) -o $(OPT_BIN) -ldl -lm

# Regression / golden test suite. Lihat tests/run.sh.
test: $(OPT_BIN)
	@NUSA=$(CURDIR)/$(OPT_BIN) ./tests/run.sh

# Tulis ulang golden -- CUMA dipakai kalau perubahan output emang disengaja.
test-update: $(OPT_BIN)
	@NUSA=$(CURDIR)/$(OPT_BIN) ./tests/run.sh --update

# Cetak sizeof(Value) dan teman-temannya -- metrik utama kerjaan optimasi ini.
sizeof:
	@printf '#include "value.hpp"\n#include <cstdio>\nint main(){printf("sizeof(Value)  = %%zu\\n", sizeof(Value));printf("alignof(Value) = %%zu\\n", alignof(Value));printf("sizeof(shared_ptr) = %%zu\\n", sizeof(std::shared_ptr<int>));printf("sizeof(std::string) = %%zu\\n", sizeof(std::string));return 0;}\n' > /tmp/nusa_sizeof.cpp
	@$(CXX) -std=c++17 -O2 -Iinclude /tmp/nusa_sizeof.cpp -o /tmp/nusa_sizeof && /tmp/nusa_sizeof

bench: $(OPT_BIN)
	@./$(OPT_BIN) run bench/microbench.ns

# Builds every plugins/<name>/<name>.cpp into plugins/<name>/<name>.so --
# each a *separate* shared object linked on its own, so a plugin's own
# dependencies (e.g. vendored SQLite, or linking a system lib like
# libcurl) never touch $(BIN) itself, keeping the core binary
# zero-external-dependency. A plugin may:
#   - vendor C sources under plugins/<name>/vendor/*.c (compiled once
#     with `cc`, cached as .o -- e.g. the SQLite amalgamation, slow to
#     recompile every time)
#   - drop extra compile flags (one line, e.g. `-I` for a vendored
#     header-only dependency's include dir) in plugins/<name>/CFLAGS,
#     applied to both the vendor .c compile and the main .cpp compile
#   - drop extra link flags (one line, e.g. `-lsqlite3` or an explicit
#     .so path when there's no unversioned dev symlink to `-l` against)
#     in plugins/<name>/LDFLAGS
# All three are optional. After building, every plugins/<name>/<name>.so
# also gets symlinked into nusantara-plugins/<name>.so (flat, next to
# this repo's own `nusantara` binary) -- that's the "system module"
# directory muat_plugin("<name>") (no path, no ".so") resolves against
# at runtime (see systemPluginDir() in src/interpreter.cpp), so
# `muat_plugin("http")` works the same as Go's `import "net/http"`
# needing no filesystem path. `nusa install` (see the install target)
# copies this same directory (resolving the symlinks) next to wherever
# `nusa` itself gets installed.
plugins:
	@mkdir -p nusantara-plugins
	@for d in $(PLUGIN_DIRS); do \
		name=$$(basename $$d); \
		src="$$d/$$name.cpp"; \
		if [ -f "$$src" ]; then \
			cflags=""; \
			[ -f "$$d/CFLAGS" ] && cflags=$$(cat "$$d/CFLAGS"); \
			vendor_objs=""; \
			for c in $$d/vendor/*.c; do \
				[ -f "$$c" ] || continue; \
				obj="$${c%.c}.o"; \
				if [ ! -f "$$obj" ] || [ "$$c" -nt "$$obj" ]; then \
					echo "$(CC) -O2 -fPIC $$cflags -c $$c -o $$obj"; \
					$(CC) -O2 -fPIC $$cflags -c "$$c" -o "$$obj" || exit 1; \
				fi; \
				vendor_objs="$$vendor_objs $$obj"; \
			done; \
			ldflags=""; \
			if [ "$(UNAME_S)" = "Darwin" ] && [ -f "$$d/LDFLAGS.darwin" ]; then \
				ldflags=$$(cat "$$d/LDFLAGS.darwin"); \
			elif [ -f "$$d/LDFLAGS" ]; then \
				ldflags=$$(cat "$$d/LDFLAGS"); \
			fi; \
			echo "$(CXX) -std=c++17 $(SHLIB_FLAG) -fPIC -O2 -Wall -Wextra -Iinclude $$cflags $$src $$vendor_objs -o $$d/$$name.$(SHLIB_EXT) $$ldflags"; \
			$(CXX) -std=c++17 $(SHLIB_FLAG) -fPIC -O2 -Wall -Wextra -Iinclude $$cflags $$src $$vendor_objs -o $$d/$$name.$(SHLIB_EXT) $$ldflags; \
			ln -sf "../$$d/$$name.$(SHLIB_EXT)" "nusantara-plugins/$$name.$(SHLIB_EXT)"; \
		fi; \
	done

clean-plugins:
	@for d in $(PLUGIN_DIRS); do rm -f $$d/*.so $$d/*.dylib $$d/vendor/*.o; done
	@rm -rf nusantara-plugins

clean: clean-plugins
	rm -f $(BIN) $(QJS_OBJS) $(BSSL_OBJS) $(QUIRC_OBJS)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

install: all plugins
	@mkdir -p $(DESTDIR)$(BINDIR)
	@cp -f $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	@ln -sf $(BIN) $(DESTDIR)$(BINDIR)/nusa
	@mkdir -p $(DESTDIR)$(BINDIR)/nusantara-plugins
	@cp -f nusantara-plugins/* $(DESTDIR)$(BINDIR)/nusantara-plugins/ 2>/dev/null || true
	@echo "Nusantara installed to $(DESTDIR)$(BINDIR)/$(BIN) and $(DESTDIR)$(BINDIR)/nusa"
