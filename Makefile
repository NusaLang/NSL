HOST_NUSA ?= $(shell if [ -x ../NSL/nusantara ]; then echo ../NSL/nusantara; elif [ -x $(HOME)/.local/bin/nusantara ]; then echo $(HOME)/.local/bin/nusantara; elif command -v nusantara >/dev/null 2>&1; then command -v nusantara; else command -v nusa 2>/dev/null; fi)
BIN := nusa
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

.PHONY: all build run clean install

all: $(BIN)

$(BIN): core/main.ns $(wildcard core/*.ns) $(wildcard core/*/*.ns)
	@echo "Building $(BIN)..."
	@if [ -n "$(HOST_NUSA)" ] && [ -x "$(HOST_NUSA)" ] && $(HOST_NUSA) -V >/dev/null 2>&1; then \
		$(HOST_NUSA) core/main.ns build -o $(BIN) . 2>/dev/null || (printf "#!/usr/bin/env nusa\n" > $(BIN) && cat core/main.ns >> $(BIN)); \
	else \
		printf "#!/usr/bin/env nusa\n" > $(BIN); \
		cat core/main.ns >> $(BIN); \
	fi
	@chmod +x $(BIN)

build: $(BIN)

run: $(BIN)
	@./$(BIN)

clean:
	rm -f $(BIN) a.out
	find . -name ".nusa-cache" -exec rm -rf {} + 2>/dev/null || true

install: $(BIN)
	@mkdir -p $(DESTDIR)$(BINDIR)
	@if [ -f "$(DESTDIR)$(BINDIR)/nusantara" ] && file "$(DESTDIR)$(BINDIR)/nusantara" | grep -q "ELF"; then \
		cp -f $(BIN) $(DESTDIR)$(BINDIR)/nusa-beta; \
		chmod +x $(DESTDIR)$(BINDIR)/nusa-beta; \
		echo "Host compiler detected; installed as $(DESTDIR)$(BINDIR)/nusa-beta"; \
	else \
		cp -f $(BIN) $(DESTDIR)$(BINDIR)/$(BIN); \
		chmod +x $(DESTDIR)$(BINDIR)/$(BIN); \
		echo "Installed to $(DESTDIR)$(BINDIR)/$(BIN)"; \
	fi
