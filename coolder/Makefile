BUILD_DIR ?= build
CMAKE ?= cmake
JOBS ?= 4
.PHONY: all configure test run clean
all: configure
	$(CMAKE) --build $(BUILD_DIR) --parallel $(JOBS)
configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release $(CMAKE_ARGS)
test: all
	ctest --test-dir $(BUILD_DIR) --output-on-failure
run: all
	./$(BUILD_DIR)/coolder
clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean
