BIN := nusa
ENGINE := nusa-engine
BOOTSTRAP_URL := https://github.com/NusaLang/NSL/releases/latest/download
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

UNAME_S := $(shell uname -s 2>/dev/null)
UNAME_M := $(shell uname -m 2>/dev/null)

ifeq ($(UNAME_S),Linux)
	ifeq ($(UNAME_M),x86_64)
		BOOTSTRAP_ASSET := nusa-linux-x86_64
	else ifeq ($(UNAME_M),aarch64)
		BOOTSTRAP_ASSET := nusa-linux-arm64
	endif
else ifeq ($(UNAME_S),Darwin)
	BOOTSTRAP_URL := https://github.com/NusaLang/NSL/releases/download/v0.0.1-build
	ifeq ($(UNAME_M),arm64)
		BOOTSTRAP_ASSET := nusa-darwin-arm64
	else
		BOOTSTRAP_ASSET := nusa-darwin-x86_64
	endif
endif

HOST_NUSA ?= $(shell if [ -x ./$(ENGINE) ]; then echo ./$(ENGINE); elif [ -x ../NSL/nusantara ]; then echo ../NSL/nusantara; elif [ -x $(HOME)/.local/bin/nusantara ]; then echo $(HOME)/.local/bin/nusantara; elif command -v nusantara >/dev/null 2>&1; then command -v nusantara; else command -v nusa 2>/dev/null; fi)

.PHONY: all build bootstrap test run clean install

all: $(BIN)

bootstrap:
	@if [ ! -x "./$(ENGINE)" ] && [ -n "$(BOOTSTRAP_ASSET)" ]; then \
		echo "Downloading bootstrap engine ($(BOOTSTRAP_ASSET))..."; \
		curl -fsSL "$(BOOTSTRAP_URL)/$(BOOTSTRAP_ASSET)" -o ./$(ENGINE) || true; \
		chmod +x ./$(ENGINE) 2>/dev/null || true; \
	fi

$(BIN): core/main.ns $(wildcard core/*.ns) $(wildcard core/*/*.ns)
	@echo "Building $(BIN)..."
	@if [ ! -x "./$(ENGINE)" ] && [ -n "$(BOOTSTRAP_ASSET)" ]; then \
		curl -fsSL "$(BOOTSTRAP_URL)/$(BOOTSTRAP_ASSET)" -o ./$(ENGINE) 2>/dev/null || true; \
		chmod +x ./$(ENGINE) 2>/dev/null || true; \
	fi
	@if [ -x "./$(ENGINE)" ] || [ -n "$(HOST_NUSA)" ]; then \
		printf '#!/bin/sh\nDIR="$$(cd "$$(dirname "$$0")" && pwd)"\nexec sh "$$DIR/core/launcher.sh" "$$DIR" "$$@"\n' > $(BIN); \
	else \
		printf "#!/usr/bin/env nusa\n" > $(BIN); \
		cat core/main.ns >> $(BIN); \
	fi
	@chmod +x $(BIN)

build: $(BIN)

test: $(BIN)
	@for f in examples/*.ns; do \
		echo "=== $$f ==="; \
		./$(BIN) $$f >/dev/null || exit 1; \
		NUSA_INTERP=1 ./$(BIN) $$f >/dev/null || exit 1; \
	done
	@echo "All examples ran."

run: $(BIN)
	@./$(BIN)

clean:
	rm -f $(BIN) $(ENGINE) a.out
	find . -name ".nusa-cache" -exec rm -rf {} + 2>/dev/null || true

install: $(BIN)
	@mkdir -p $(DESTDIR)$(BINDIR)
	@if [ -x "./$(ENGINE)" ]; then \
		mkdir -p $(DESTDIR)$(PREFIX)/share/nusa; \
		cp -f ./$(ENGINE) $(DESTDIR)$(PREFIX)/share/nusa/$(ENGINE); \
		chmod +x $(DESTDIR)$(PREFIX)/share/nusa/$(ENGINE); \
		cp -r core compiler std net crypto media lib Makefile README.md LICENSE NOTICE $(DESTDIR)$(PREFIX)/share/nusa/ 2>/dev/null || true; \
		printf '#!/bin/sh\nexec "%s/nusa" "$$@"\n' "$(DESTDIR)$(PREFIX)/share/nusa" > $(DESTDIR)$(BINDIR)/$(BIN); \
		printf '#!/bin/sh\nDIR="$$(cd "$$(dirname "$$0")" && pwd)"\nexec sh "$$DIR/core/launcher.sh" "$$DIR" "$$@"\n' > $(DESTDIR)$(PREFIX)/share/nusa/$(BIN); \
		chmod +x $(DESTDIR)$(BINDIR)/$(BIN) $(DESTDIR)$(PREFIX)/share/nusa/$(BIN); \
		echo "Installed to $(DESTDIR)$(BINDIR)/$(BIN) (runtime at $(DESTDIR)$(PREFIX)/share/nusa)"; \
	elif [ -f "$(DESTDIR)$(BINDIR)/nusantara" ] && file "$(DESTDIR)$(BINDIR)/nusantara" | grep -q "ELF"; then \
		cp -f $(BIN) $(DESTDIR)$(BINDIR)/nusa-beta; \
		chmod +x $(DESTDIR)$(BINDIR)/nusa-beta; \
		echo "Host compiler detected; installed as $(DESTDIR)$(BINDIR)/nusa-beta"; \
	else \
		cp -f $(BIN) $(DESTDIR)$(BINDIR)/$(BIN); \
		chmod +x $(DESTDIR)$(BINDIR)/$(BIN); \
		echo "Installed to $(DESTDIR)$(BINDIR)/$(BIN)"; \
	fi
