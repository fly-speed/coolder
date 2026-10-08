BUILD_DIR ?= build
CMAKE ?= cmake
JOBS ?= 4
ifneq ($(strip $(HAS_IO_URING)),)
CMAKE_OPTIONS += -DHAS_IO_URING=$(HAS_IO_URING)
endif
.PHONY: all configure test run clean
all: configure
	$(CMAKE) --build $(BUILD_DIR) --parallel $(JOBS)
configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release $(CMAKE_ARGS) $(CMAKE_OPTIONS)
test: all
	ctest --test-dir $(BUILD_DIR) --output-on-failure
run: all
	./$(BUILD_DIR)/coolder
clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean
