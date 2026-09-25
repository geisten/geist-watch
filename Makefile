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

# Benchmark harness. Not part of libgeist_watch.a: scoring a run is a
# measurement concern, not product runtime, and the library stays the
# model-free state core the plan describes.
BENCH_SRC   := src/gw_manifest.c src/gw_score.c
BENCH_SCENES := $(wildcard benchmarks/scenes/*.scene)

# Rule layer. Also outside libgeist_watch.a: sentences, regions and the
# fixed DE/EN wordings are the product's surface, while the library is
# the model-free timing core and nothing else.
RULE_SRC := src/gw_rules.c

# Benchmark report and the release gate. Tool-side like the harness: grading
# a run is a measurement concern, not product runtime.
REPORT_SRC := src/gw_report.c

TEST_SRC  := $(wildcard tests/test_*.c)
TEST_BINS := $(TEST_SRC:tests/%.c=$(BUILD)/%)

# Shell tests cover what is written in shell: the model setup verifies and
# installs files, and a C test cannot exercise a rename or a staging
# directory that must not survive a failure.
TEST_SH := $(wildcard tests/test_*.sh)

COMPILE = $(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude -Isrc

.PHONY: all lib check test check-headers format format-check analyze clean help print-config bench setup report-check

all: lib

lib: $(LIB)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(COMPILE) -c $< -o $@

$(LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%: tests/%.c $(CORE_SRC) $(BENCH_SRC) | $(BUILD)
	$(COMPILE) -Itests $< $(CORE_SRC) $(BENCH_SRC) $(RULE_SRC) $(REPORT_SRC) $(LDFLAGS) $(SAN_FLAGS) $(PROJECT_LIBS) -lm -o $@

$(BUILD)/replay: benchmarks/replay.c $(CORE_SRC) $(BENCH_SRC) | $(BUILD)
	$(COMPILE) $^ $(LDFLAGS) $(SAN_FLAGS) $(PROJECT_LIBS) -lm -o $@

$(BUILD)/report_check: benchmarks/report_check.c $(REPORT_SRC) $(CORE_SRC) | $(BUILD)
	$(COMPILE) $^ $(LDFLAGS) $(SAN_FLAGS) $(PROJECT_LIBS) -lm -o $@

# Replay every scene and report. Model-free: the scenes carry labels that
# stand in for the model, so this runs with no engine and no footage.
# Floors are the plan's v0.1 targets, and are targets rather than
# measurements until real scenes replace the synthetic ones.
bench: $(BUILD)/replay
	$(BUILD)/replay --min-precision 0.95 --min-recall 0.90 $(BENCH_SCENES)

# Explicit model acquisition. Never a dependency of anything: a build, a
# test or a benchmark that quietly downloaded gigabytes would make an
# offline machine look broken and a metered connection expensive. MODEL
# names a manifest under tools/models/; FROM installs from a local
# directory, which is what makes a fully net-free setup possible.
#
#   make setup MODEL=smolvlm-500m
#   make setup MODEL=smolvlm-500m FROM=/media/weights/smolvlm
setup:
	@test -n "$(MODEL)" || { echo "usage: make setup MODEL=<name> [FROM=<dir>]"; \
	    echo "available:"; \
	    if ls tools/models/*.model >/dev/null 2>&1; then \
	        ls tools/models/*.model | sed 's|.*/||;s|\.model$$||;s|^|  |'; \
	    else \
	        echo "  (none pinned yet — see tools/pin-model.sh)"; \
	    fi; exit 2; }
	sh tools/fetch-model.sh "$(MODEL)" $(if $(FROM),--from "$(FROM)",)

# The model-free gate. This is what must pass on every supported platform
# with no engine, no model and no camera present.
# Grade a benchmark report against the frozen v0.1 criteria. The criteria
# are committed before any hardware number exists, so they cannot be tuned
# to fit one.
#
#   make report-check REPORT=runs/pi5-2026-09-26.report
report-check: $(BUILD)/report_check
	@test -n "$(REPORT)" || { echo "usage: make report-check REPORT=<file>"; exit 2; }
	$(BUILD)/report_check --criteria benchmarks/criteria/v0.1.criteria "$(REPORT)"

check: test check-headers

test: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
	    printf '%-28s ' "$$(basename $$t)"; \
	    if $$t; then echo PASS; else echo FAIL; fail=1; fi; \
	done; \
	for t in $(TEST_SH); do \
	    printf '%-28s ' "$$(basename $$t)"; \
	    if sh $$t; then echo PASS; else echo FAIL; fail=1; fi; \
	done; \
	exit $$fail

# The public header has to stand on its own, and stay consumable from C++.
check-headers: | $(BUILD)
	printf '#include "geist_watch.h"\nint main(void) { return 0; }\n' \
	    | $(COMPILE) -x c - -o $(BUILD)/header-c $(SAN_FLAGS)
	printf '#include "geist_watch.h"\nint main() { return 0; }\n' \
	    | $(CXX) -std=c++17 -Wall -Wextra -pedantic-errors -Iinclude -x c++ - -o $(BUILD)/header-cxx

format:
	clang-format -i include/*.h src/*.h src/*.c tests/*.c tests/*.h benchmarks/*.c

format-check:
	@clang-format --dry-run --Werror include/*.h src/*.h src/*.c tests/*.c tests/*.h benchmarks/*.c

# One file at a time: `clang --analyze` writes a report per input and
# refuses a single -o for several of them. This covers whatever the tree
# has, library or not.
analyze:
	@for f in $(CORE_SRC) $(BENCH_SRC) $(RULE_SRC) $(REPORT_SRC); do \
	    echo "  analyze $$f"; \
	    $(CC) --analyze $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude -Isrc $$f -o /dev/null || exit 1; \
	done

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
	@echo "  make bench           replay the scenes and report TP/FP/FN"
	@echo "  make setup MODEL=x   install a pinned model (never automatic)"
	@echo "  make report-check    grade a benchmark report against v0.1 criteria"
	@echo "  make analyze         clang static analysis"
	@echo "  make print-config    effective configuration"
	@echo ""
	@echo "  TARGET=$(VALID_TARGETS)"
	@echo "  MODE=$(VALID_MODES)"
	@echo ""
	@echo "Targets needing the engine, a model or a camera (deps, setup,"
	@echo "test-e2e, bench, bench-pi, packaging) land with their milestone."
