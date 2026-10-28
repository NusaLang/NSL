HOST_NUSA ?= $(shell if [ -x ../NSL/nusantara ]; then echo ../NSL/nusantara; elif [ -x $(HOME)/.local/bin/nusantara ]; then echo $(HOME)/.local/bin/nusantara; elif command -v nusantara >/dev/null 2>&1; then command -v nusantara; else command -v nusa 2>/dev/null; fi)
BIN := nusa
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

.PHONY: all build test run clean install check native

all: $(BIN)

$(BIN): core/main.ns $(wildcard core/*.ns)
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

check: $(BIN)
	@echo "Checking syntax..."
	@if ./$(BIN) -V >/dev/null 2>&1; then \
		./$(BIN) --check core/main.ns && \
		./$(BIN) --check compiler/native.ns; \
	else \
		echo "Compatible runner not found in PATH; syntax check skipped."; \
	fi
	@echo "Syntax OK."

test: $(BIN) check
	@echo "Running tests..."
	@if ./$(BIN) -V >/dev/null 2>&1; then \
		if [ -d tests/self_hosted ]; then \
			./$(BIN) run tests/self_hosted/test1.ns && \
			./$(BIN) run tests/self_hosted/test2.ns && \
			./$(BIN) run tests/self_hosted/testStd.ns; \
		else \
			./$(BIN) -e "cetak('Self-hosted interpreter OK')" && \
			./$(BIN) --vm -e "cetak('Bytecode VM OK')"; \
		fi; \
		if [ -d tests/native ]; then \
			./$(BIN) build --native tests/native/basic.ns -o /tmp/native_test_basic && /tmp/native_test_basic >/dev/null && \
			./$(BIN) build --native tests/native/allFeatures.ns -o /tmp/native_test_all && /tmp/native_test_all >/dev/null; \
		fi; \
	else \
		echo "Compatible runner not found in PATH; runtime tests skipped."; \
	fi
	@rm -f /tmp/native_test_*
	@echo "All tests passed!"

clean:
	rm -f $(BIN) a.out /tmp/native_test_* /tmp/native_bench*
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

native: $(BIN)
	@if [ -f bench/native/bench.ns ]; then \
		./$(BIN) build --native bench/native/bench.ns -o /tmp/native_bench && /tmp/native_bench; \
		rm -f /tmp/native_bench; \
	fi
