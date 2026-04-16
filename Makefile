BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
CMAKE ?= cmake
CTEST ?= ctest

# Forwarded to configure, e.g. `make configure EXTRA_CMAKE_ARGS="-DDLMALLOC_SOURCE=/path"`
EXTRA_CMAKE_ARGS ?=

.PHONY: all configure build test test-all unit integration clean rebuild help

all: test

help:
	@echo "Targets:"
	@echo "  make configure    - run cmake configure into $(BUILD_DIR)"
	@echo "  make build        - configure + build everything"
	@echo "  make test         - build + run our ctest suite"
	@echo "  make test-all     - build + run ctest including rlbox framework tests"
	@echo "  make unit         - build + run unit tests only"
	@echo "  make integration  - build + run integration tests only"
	@echo "  make clean        - remove $(BUILD_DIR)"
	@echo "  make rebuild      - clean + build"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_DIR=$(BUILD_DIR)"
	@echo "  BUILD_TYPE=$(BUILD_TYPE)   (Release, Debug, RelWithDebInfo, ...)"
	@echo "  JOBS=$(JOBS)"
	@echo "  EXTRA_CMAKE_ARGS=$(EXTRA_CMAKE_ARGS)"

$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(EXTRA_CMAKE_ARGS)

configure: $(BUILD_DIR)/CMakeCache.txt

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS)

test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure -R "^process_sandbox:"

# Run the full ctest suite including transitively-fetched rlbox tests.
# Noticeably slower — prefer `make test` for day-to-day work.
test-all: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

unit: build
	cd $(BUILD_DIR) && ./test_process_sandbox "[mem],[pointers]"

integration: build
	cd $(BUILD_DIR) && ./test_process_sandbox "[sandbox]"

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean build
