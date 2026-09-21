# geist-watch — tiny local vision agent.
#
# Every target here is model-free and offline. Targets that need the
# engine, a model or a camera (deps, setup, test-e2e, bench, bench-pi,
# packaging) arrive with the milestones that introduce them; the project
# plan asks that empty subsystems not be created before their milestone
# begins, so they are absent rather than stubbed.

include mk/config.mk

CORE_SRC := src/gw_state.c src/gw_event.c
CORE_OBJ := $(CORE_SRC:src/%.c=$(BUILD)/%.o)

TEST_SRC  := $(wildcard tests/test_*.c)
TEST_BINS := $(TEST_SRC:tests/%.c=$(BUILD)/%)

COMPILE = $(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude -Isrc

.PHONY: all lib check test check-headers format format-check analyze clean help print-config

all: lib

lib: $(LIB)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(COMPILE) -c $< -o $@

$(LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%: tests/%.c $(CORE_SRC) | $(BUILD)
	$(COMPILE) -Itests $< $(CORE_SRC) $(LDFLAGS) $(SAN_FLAGS) $(PROJECT_LIBS) -o $@

# The model-free gate. This is what must pass on every supported platform
# with no engine, no model and no camera present.
check: test check-headers

test: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
	    printf '%-28s ' "$$(basename $$t)"; \
	    if $$t; then echo PASS; else echo FAIL; fail=1; fi; \
	done; \
	exit $$fail

# The public header has to stand on its own, and stay consumable from C++.
check-headers: | $(BUILD)
	printf '#include "geist_watch.h"\nint main(void) { return 0; }\n' \
	    | $(COMPILE) -x c - -o $(BUILD)/header-c $(SAN_FLAGS)
	printf '#include "geist_watch.h"\nint main() { return 0; }\n' \
	    | $(CXX) -std=c++17 -Wall -Wextra -pedantic-errors -Iinclude -x c++ - -o $(BUILD)/header-cxx

format:
	clang-format -i include/*.h src/*.c tests/*.c tests/*.h

format-check:
	@clang-format --dry-run --Werror include/*.h src/*.c tests/*.c tests/*.h

analyze:
	$(CC) --analyze $(CPPFLAGS) $(BASE_FLAGS) -Iinclude -Isrc $(CORE_SRC) -o /dev/null

clean:
	rm -rf $(BUILD)

print-config:
	@echo "TARGET   = $(TARGET)"
	@echo "MODE     = $(MODE)"
	@echo "CC       = $(CC) ($(COMPILER_TARGET))"
	@echo "BUILD    = $(BUILD)"
	@echo "WARN     = $(WARN)"

help:
	@echo "geist-watch — model-free targets (no engine, model or camera needed)"
	@echo "  make lib             build libgeist_watch.a"
	@echo "  make check           model-free C tests plus the header contract"
	@echo "  make MODE=asan check same, under ASan/UBSan"
	@echo "  make format-check    clang-format, no changes made"
	@echo "  make analyze         clang static analysis"
	@echo "  make print-config    effective configuration"
	@echo ""
	@echo "  TARGET=$(VALID_TARGETS)"
	@echo "  MODE=$(VALID_MODES)"
	@echo ""
	@echo "Targets needing the engine, a model or a camera (deps, setup,"
	@echo "test-e2e, bench, bench-pi, packaging) land with their milestone."
