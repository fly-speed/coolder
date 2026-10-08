# Standalone build. Pass ACL_PATH / OPENSSL_PATH as needed.
BUILD_DIR ?= build
CMAKE ?= cmake
MODULE_DIRS := agent provider storage project workspace sandbox context prompt validation common runtime
# Set by webcool's wrapper so a fresh build and cached rebuild use the same ABI.
ifeq ($(shell uname -s),Darwin)
ifneq ($(strip $(MACOSX_DEPLOYMENT_TARGET)),)
CMAKE_OPTIONS += -DCMAKE_OSX_DEPLOYMENT_TARGET=$(MACOSX_DEPLOYMENT_TARGET)
endif
ifneq ($(strip $(MACOS_ARCHITECTURES)),)
CMAKE_OPTIONS += -DCMAKE_OSX_ARCHITECTURES="$(MACOS_ARCHITECTURES)"
endif
endif
.PHONY: all clean install
all:
	$(CMAKE) -S . -B $(BUILD_DIR) $(if $(ACL_PATH),-DACL_PATH="$(ACL_PATH)") $(if $(OPENSSL_PATH),-DOPENSSL_PATH="$(OPENSSL_PATH)") $(CMAKE_OPTIONS) $(CMAKE_ARGS)
	+$(CMAKE) --build $(BUILD_DIR)
install: all
	$(CMAKE) --install $(BUILD_DIR) $(if $(PREFIX),--prefix "$(PREFIX)")
# webcool's Make build writes objects beside the library sources.
# Clean these even when the standalone CMake build has not been configured.
clean:
	$(RM) *.o *.obj *.d libai.a libai.lib $(foreach module,$(MODULE_DIRS),$(wildcard $(module)/*.o $(module)/*.obj $(module)/*.d))
	@if [ -f "$(BUILD_DIR)/CMakeCache.txt" ]; then \
		$(CMAKE) --build "$(BUILD_DIR)" --target clean; \
	fi
