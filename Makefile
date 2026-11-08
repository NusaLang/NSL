HOST_NUSA ?= $(shell if [ -x ./nusa-engine ]; then echo ./nusa-engine; elif [ -x engine/nusantara ]; then echo engine/nusantara; elif [ -x ../NSL/nusantara ]; then echo ../NSL/nusantara; elif [ -x $(HOME)/.local/bin/nusantara ]; then echo $(HOME)/.local/bin/nusantara; elif command -v nusantara >/dev/null 2>&1; then command -v nusantara; else command -v nusa 2>/dev/null; fi)
BIN := nusa
ENGINE := nusa-engine
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

.PHONY: all build engine run clean install

all: $(BIN)

engine:
	@if [ -d engine ]; then \
		$(MAKE) -C engine nusantara; \
		cp -f engine/nusantara $(ENGINE); \
		chmod +x $(ENGINE); \
	fi

$(BIN): core/main.ns $(wildcard core/*.ns) $(wildcard core/*/*.ns)
	@echo "Building $(BIN)..."
	@if [ ! -x "./$(ENGINE)" ] && [ ! -x "engine/nusantara" ] && [ -d "engine" ]; then \
		$(MAKE) -C engine nusantara >/dev/null 2>&1 || true; \
	fi
	@if [ -x "engine/nusantara" ] && [ ! -x "./$(ENGINE)" ]; then \
		cp -f engine/nusantara ./$(ENGINE); \
		chmod +x ./$(ENGINE); \
	fi
	@if [ -x "./$(ENGINE)" ]; then \
		printf '#!/bin/sh\nDIR="$$(cd "$$(dirname "$$0")" && pwd)"\nif [ -x "$$DIR/$(ENGINE)" ]; then\n    exec "$$DIR/$(ENGINE)" "$$DIR/core/main.ns" "$$@"\nfi\nexec nusa "$$DIR/core/main.ns" "$$@"\n' > $(BIN); \
	elif [ -n "$(HOST_NUSA)" ] && [ -x "$(HOST_NUSA)" ] && $(HOST_NUSA) -V >/dev/null 2>&1; then \
		$(HOST_NUSA) core/main.ns build -o $(BIN) . 2>/dev/null || (printf "#!/usr/bin/env nusa\n" > $(BIN) && cat core/main.ns >> $(BIN)); \
	else \
		printf "#!/usr/bin/env nusa\n" > $(BIN); \
		cat core/main.ns >> $(BIN); \
	fi
	@chmod +x $(BIN)

build: $(BIN)

test: $(BIN)
	@echo "Running tests..."
	@./$(BIN) -c "print('Self-hosted interpreter OK')"
	@./$(BIN) --vm -c "print('Bytecode VM OK')"
	@if [ -d tests/self_hosted ]; then \
		./$(BIN) run tests/self_hosted/test1.ns && \
		./$(BIN) run tests/self_hosted/test2.ns && \
		./$(BIN) run tests/self_hosted/testStd.ns; \
	fi
	@echo "All tests passed!"

run: $(BIN)
	@./$(BIN)

clean:
	rm -f $(BIN) $(ENGINE) a.out
	@if [ -d engine ]; then $(MAKE) -C engine clean >/dev/null 2>&1 || true; fi
	find . -name ".nusa-cache" -exec rm -rf {} + 2>/dev/null || true

install: $(BIN)
	@mkdir -p $(DESTDIR)$(BINDIR)
	@if [ -x "./$(ENGINE)" ]; then \
		mkdir -p $(DESTDIR)$(PREFIX)/share/nusa; \
		cp -f ./$(ENGINE) $(DESTDIR)$(PREFIX)/share/nusa/$(ENGINE); \
		chmod +x $(DESTDIR)$(PREFIX)/share/nusa/$(ENGINE); \
		cp -r core compiler std net crypto media lib Makefile README.md LICENSE NOTICE $(DESTDIR)$(PREFIX)/share/nusa/ 2>/dev/null || true; \
		printf '#!/bin/sh\nexec "%s/nusa" "$$@"\n' "$(DESTDIR)$(PREFIX)/share/nusa" > $(DESTDIR)$(BINDIR)/$(BIN); \
		printf '#!/bin/sh\nDIR="$$(cd "$$(dirname "$$0")" && pwd)"\nexec "$$DIR/$(ENGINE)" "$$DIR/core/main.ns" "$$@"\n' > $(DESTDIR)$(PREFIX)/share/nusa/$(BIN); \
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
