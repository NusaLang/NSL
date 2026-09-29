CXX ?= g++
CC ?= cc
CXXFLAGS := -std=c++17 -O3 -flto=auto -DNDEBUG -Wall -Wextra -Iinclude -pthread
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

.PHONY: all clean run plugins clean-plugins test test-update opt sizeof bench

all: $(BIN) plugins

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN) -ldl -lm

run: $(BIN)
	./$(BIN) run examples/hello.ns

# --- build terpisah buat kerjaan optimasi -----------------------------------
# JANGAN PERNAH nimpa ~/.local/bin/nusa: itu dipakai service produksi yang
# lagi jalan (bot WhatsApp, backend, registry). Semua eksperimen dibangun
# ke build-opt/nusa, dan test runner default-nya nunjuk ke situ.
OPT_BIN := build-opt/nusa

opt: $(OPT_BIN) plugins

$(OPT_BIN): $(SRC) $(wildcard include/*.hpp)
	@mkdir -p build-opt
	@ln -sfn ../nusantara-plugins build-opt/nusantara-plugins
	$(CXX) $(CXXFLAGS) $(SRC) -o $(OPT_BIN) -ldl -lm

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
			extra=""; \
			[ -f "$$d/SOURCES" ] && extra=$$(cat "$$d/SOURCES"); \
			ldflags=""; \
			if [ "$(UNAME_S)" = "Darwin" ] && [ -f "$$d/LDFLAGS.darwin" ]; then \
				ldflags=$$(cat "$$d/LDFLAGS.darwin"); \
			elif [ -f "$$d/LDFLAGS" ]; then \
				ldflags=$$(cat "$$d/LDFLAGS"); \
			fi; \
			echo "$(CXX) -std=c++17 $(SHLIB_FLAG) -fPIC -O2 -Wall -Wextra -Iinclude $$cflags $$src $$extra $$vendor_objs -o $$d/$$name.$(SHLIB_EXT) $$ldflags"; \
			$(CXX) -std=c++17 $(SHLIB_FLAG) -fPIC -O2 -Wall -Wextra -Iinclude $$cflags $$src $$extra $$vendor_objs -o $$d/$$name.$(SHLIB_EXT) $$ldflags; \
			ln -sf "../$$d/$$name.$(SHLIB_EXT)" "nusantara-plugins/$$name.$(SHLIB_EXT)"; \
		fi; \
	done

clean-plugins:
	@for d in $(PLUGIN_DIRS); do rm -f $$d/*.so $$d/*.dylib $$d/vendor/*.o; done
	@rm -rf nusantara-plugins

clean: clean-plugins
	rm -f $(BIN)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

install: all plugins
	@mkdir -p $(DESTDIR)$(BINDIR)
	@cp -f $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	@ln -sf $(BIN) $(DESTDIR)$(BINDIR)/nusa
	@mkdir -p $(DESTDIR)$(BINDIR)/nusantara-plugins
	@cp -f nusantara-plugins/* $(DESTDIR)$(BINDIR)/nusantara-plugins/ 2>/dev/null || true
	@echo "Nusantara installed to $(DESTDIR)$(BINDIR)/$(BIN) and $(DESTDIR)$(BINDIR)/nusa"

# --- test kripto TLS bawaan (vektor dari implementasi independen, lihat tests/tls/) ---
TLS_CRYPTO_SRC := src/tls_bigint_hash.cpp src/tls_cipher_sig.cpp
build-opt/tls_selftest: tests/tls_selftest.cpp $(TLS_CRYPTO_SRC) include/tls_crypto.hpp
	@mkdir -p build-opt
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Iinclude tests/tls_selftest.cpp $(TLS_CRYPTO_SRC) -o build-opt/tls_selftest

tls-test: build-opt/tls_selftest
	./build-opt/tls_selftest tests/tls/vectors.txt

TLS_SRC := src/tls.cpp src/tls_x509.cpp src/tls_ca_bundle.cpp $(TLS_CRYPTO_SRC)
build-opt/tls_client: tests/tls_client.cpp $(TLS_SRC) include/tls.hpp include/tls_x509.hpp include/tls_crypto.hpp
	@mkdir -p build-opt
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Iinclude tests/tls_client.cpp $(TLS_SRC) -o build-opt/tls_client
