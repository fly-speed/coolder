.DEFAULT_GOAL := all
.PHONY: all acl-lib zlib-lib zlib-clean openssl-lib openssl-clean openssl-perl-check \
	acl-strip-fat-archives clean

OSNAME       := $(shell uname -s)
ACL_FLAG     := OPENSSL_STATIC=yes
ZLIB_CC      ?= cc
ZLIB_CFLAGS  ?= -O3
ZLIB_ROOT    := zlib-1.2.11
ZLIB_BUILD   := .build/zlib
ZLIB_PREFIX  := $(abspath $(ZLIB_ROOT)/install)
ACL_PREBUILD :=
ACL_OPENSSL_STATIC_STAMP := .build/acl-openssl-static

OPENSSL_VERSION      := 1.1.1q
OPENSSL_ARCHIVE      := openssl-$(OPENSSL_VERSION).tar.gz
OPENSSL_ROOT         := openssl
OPENSSL_SOURCE       := $(OPENSSL_ROOT)/src
OPENSSL_SOURCE_STAMP := $(OPENSSL_SOURCE)/.extracted
OPENSSL_BUILD_ROOT   := $(OPENSSL_ROOT)/build
OPENSSL_INSTALL_ROOT := $(OPENSSL_ROOT)/install
OPENSSL_PREFIX       := $(abspath $(OPENSSL_ROOT))
OPENSSL_PERL         ?= perl
# Modules used by Configure and by the Perl helpers invoked by make/install_dev.
OPENSSL_PERL_MODULES := Config FindBin File::Basename File::Spec::Functions \
	File::Path File::Glob File::Compare File::Copy Getopt::Std Getopt::Long \
	Pod::Usage Fcntl Cwd Exporter
OPENSSL_JOBS         ?= $(shell sysctl -n hw.ncpu 2>/dev/null || \
	getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
MACOSX_DEPLOYMENT_TARGET ?= 13.0

ifeq ($(findstring Darwin, $(OSNAME)), Darwin)
	export MACOSX_DEPLOYMENT_TARGET
	ACL_FLAG     += ENV_CC="clang -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET) -arch arm64 -arch x86_64" \
		ENV_CPP="clang++ -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET) -arch arm64 -arch x86_64"
	ZLIB_CC      := clang
	ZLIB_CFLAGS  += -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET) -arch arm64 -arch x86_64
	# macOS ar(1) cannot update existing fat static libraries.
	ACL_PREBUILD := acl-strip-fat-archives
endif

all: acl-lib zlib-lib openssl-lib

acl-strip-fat-archives: $(ACL_OPENSSL_STATIC_STAMP)
	@find acl -path '*/lib/*.a' -delete 2>/dev/null || true

acl-lib: $(ACL_OPENSSL_STATIC_STAMP) $(ACL_PREBUILD)
	$(MAKE) -C acl all_lib $(ACL_FLAG)

# Configure out of tree so the vendored source and VS headers stay unchanged.
zlib-lib:
	mkdir -p $(ZLIB_BUILD)
	cd $(ZLIB_BUILD) && CC="$(ZLIB_CC)" CFLAGS="$(ZLIB_CFLAGS)" \
		sh "$(abspath $(ZLIB_ROOT))/configure" --static --prefix="$(ZLIB_PREFIX)"
	$(MAKE) -C $(ZLIB_BUILD) -j$(OPENSSL_JOBS)
	$(MAKE) -C $(ZLIB_BUILD) install

zlib-clean:
	rm -rf $(ZLIB_BUILD) $(ZLIB_ROOT)/install

# OpenSSL 1.1.1's Configure script requires Perl modules which may be omitted
# from minimal Linux installations.  Check before removing an existing build.
openssl-perl-check:
	@command -v "$(OPENSSL_PERL)" >/dev/null 2>&1 || { \
		echo "ERROR: Perl executable not found: $(OPENSSL_PERL)"; \
		echo "Install Perl or run make OPENSSL_PERL=/path/to/perl"; \
		exit 1; \
	}
	@missing=""; \
	for module in $(OPENSSL_PERL_MODULES); do \
		if ! "$(OPENSSL_PERL)" -M$$module -e 1 >/dev/null 2>&1; then \
			missing="$$missing $$module"; \
		fi; \
	done; \
	if test -n "$$missing"; then \
		echo "ERROR: Perl is missing modules required to configure/build OpenSSL:$$missing"; \
		echo "On AlmaLinux/RHEL/Rocky Linux, install the complete core Perl package with:"; \
		echo "  sudo dnf install -y perl"; \
		echo "For only File::Compare, use: sudo dnf install -y perl-File-Compare"; \
		echo "Note: File::Spec is provided by perl-PathTools, not perl-File-Spec."; \
		echo "Or run make OPENSSL_PERL=/path/to/a/complete/perl"; \
		exit 1; \
	fi

# Extract into a stable source directory.  The archive remains untouched, and
# changing it causes both architecture builds to be regenerated.
$(OPENSSL_SOURCE_STAMP): $(OPENSSL_ARCHIVE)
	@echo "Extracting OpenSSL $(OPENSSL_VERSION)"
	rm -rf $(OPENSSL_SOURCE)
	mkdir -p $(OPENSSL_SOURCE)
	tar -xzf $(OPENSSL_ARCHIVE) -C $(OPENSSL_SOURCE) --strip-components=1
	@touch $@

ifeq ($(findstring Darwin, $(OSNAME)), Darwin)

OPENSSL_X86_STAMP := $(OPENSSL_INSTALL_ROOT)/x86_64/.installed
OPENSSL_ARM_STAMP := $(OPENSSL_INSTALL_ROOT)/arm64/.installed
OPENSSL_UNIVERSAL_STAMP := $(OPENSSL_ROOT)/lib/.static-universal
OPENSSL_READY_STAMP := $(OPENSSL_UNIVERSAL_STAMP)

$(OPENSSL_X86_STAMP): $(OPENSSL_SOURCE_STAMP) Makefile
	@$(MAKE) --no-print-directory openssl-perl-check OPENSSL_PERL="$(OPENSSL_PERL)"
	@echo "Building OpenSSL $(OPENSSL_VERSION) for macOS x86_64"
	rm -rf $(OPENSSL_BUILD_ROOT)/x86_64 $(OPENSSL_INSTALL_ROOT)/x86_64
	mkdir -p $(OPENSSL_BUILD_ROOT)/x86_64 $(OPENSSL_INSTALL_ROOT)/x86_64
	cd $(OPENSSL_BUILD_ROOT)/x86_64 && \
		CC=clang "$(OPENSSL_PERL)" $(abspath $(OPENSSL_SOURCE))/Configure \
			darwin64-x86_64-cc no-shared no-tests \
			-mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET) \
			--prefix=$(abspath $(OPENSSL_INSTALL_ROOT)/x86_64) \
			--openssldir=$(abspath $(OPENSSL_INSTALL_ROOT)/x86_64/ssl)
	$(MAKE) -C $(OPENSSL_BUILD_ROOT)/x86_64 -j$(OPENSSL_JOBS)
	$(MAKE) -C $(OPENSSL_BUILD_ROOT)/x86_64 install_dev
	@touch $@

$(OPENSSL_ARM_STAMP): $(OPENSSL_SOURCE_STAMP) Makefile
	@$(MAKE) --no-print-directory openssl-perl-check OPENSSL_PERL="$(OPENSSL_PERL)"
	@echo "Building OpenSSL $(OPENSSL_VERSION) for macOS arm64"
	rm -rf $(OPENSSL_BUILD_ROOT)/arm64 $(OPENSSL_INSTALL_ROOT)/arm64
	mkdir -p $(OPENSSL_BUILD_ROOT)/arm64 $(OPENSSL_INSTALL_ROOT)/arm64
	cd $(OPENSSL_BUILD_ROOT)/arm64 && \
		CC=clang "$(OPENSSL_PERL)" $(abspath $(OPENSSL_SOURCE))/Configure \
			darwin64-arm64-cc no-shared no-tests \
			-mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET) \
			--prefix=$(abspath $(OPENSSL_INSTALL_ROOT)/arm64) \
			--openssldir=$(abspath $(OPENSSL_INSTALL_ROOT)/arm64/ssl)
	$(MAKE) -C $(OPENSSL_BUILD_ROOT)/arm64 -j$(OPENSSL_JOBS)
	$(MAKE) -C $(OPENSSL_BUILD_ROOT)/arm64 install_dev
	@touch $@

$(OPENSSL_UNIVERSAL_STAMP): $(OPENSSL_X86_STAMP) $(OPENSSL_ARM_STAMP)
	@echo "Creating universal static OpenSSL libraries (x86_64 + arm64)"
	rm -rf $(OPENSSL_ROOT)/include $(OPENSSL_ROOT)/lib $(OPENSSL_ROOT)/bin
	mkdir -p $(OPENSSL_ROOT)/include $(OPENSSL_ROOT)/lib
	cp -R $(OPENSSL_INSTALL_ROOT)/arm64/include/openssl \
		$(OPENSSL_ROOT)/include/
	lipo -create \
		$(OPENSSL_INSTALL_ROOT)/x86_64/lib/libcrypto.a \
		$(OPENSSL_INSTALL_ROOT)/arm64/lib/libcrypto.a \
		-output $(OPENSSL_ROOT)/lib/libcrypto.a
	lipo -create \
		$(OPENSSL_INSTALL_ROOT)/x86_64/lib/libssl.a \
		$(OPENSSL_INSTALL_ROOT)/arm64/lib/libssl.a \
		-output $(OPENSSL_ROOT)/lib/libssl.a
	@touch $@

openssl-lib: $(OPENSSL_UNIVERSAL_STAMP)
	@echo "OpenSSL static libraries: $(abspath $(OPENSSL_ROOT)/lib)"
	@lipo -archs $(OPENSSL_ROOT)/lib/libcrypto.a
	@lipo -archs $(OPENSSL_ROOT)/lib/libssl.a

else

OPENSSL_NATIVE_STAMP := $(OPENSSL_ROOT)/.native-installed
OPENSSL_READY_STAMP := $(OPENSSL_NATIVE_STAMP)

$(OPENSSL_NATIVE_STAMP): $(OPENSSL_SOURCE_STAMP) Makefile
	@$(MAKE) --no-print-directory openssl-perl-check OPENSSL_PERL="$(OPENSSL_PERL)"
	@echo "Building OpenSSL $(OPENSSL_VERSION) for $(OSNAME)/$(shell uname -m)"
	rm -rf $(OPENSSL_BUILD_ROOT)/native $(OPENSSL_ROOT)/include \
		$(OPENSSL_ROOT)/lib $(OPENSSL_ROOT)/bin $(OPENSSL_ROOT)/ssl
	mkdir -p $(OPENSSL_BUILD_ROOT)/native
	cd $(OPENSSL_BUILD_ROOT)/native && \
		PERL="$(OPENSSL_PERL)" $(abspath $(OPENSSL_SOURCE))/config no-shared no-tests \
			--prefix=$(OPENSSL_PREFIX) \
			--openssldir=$(OPENSSL_PREFIX)/ssl
	$(MAKE) -C $(OPENSSL_BUILD_ROOT)/native -j$(OPENSSL_JOBS)
	$(MAKE) -C $(OPENSSL_BUILD_ROOT)/native install_dev
	@touch $@

openssl-lib: $(OPENSSL_NATIVE_STAMP)
	@echo "OpenSSL static libraries: $(abspath $(OPENSSL_ROOT)/lib)"

endif

$(ACL_OPENSSL_STATIC_STAMP): $(OPENSSL_READY_STAMP)
	@echo "Preparing ACL for static OpenSSL and macOS deployment target $(MACOSX_DEPLOYMENT_TARGET)"
	$(MAKE) -C acl clean
	@mkdir -p $(dir $@)
	@touch $@

openssl-clean:
	rm -rf $(OPENSSL_SOURCE) $(OPENSSL_BUILD_ROOT) \
		$(OPENSSL_INSTALL_ROOT) $(OPENSSL_ROOT)/include \
		$(OPENSSL_ROOT)/lib $(OPENSSL_ROOT)/bin $(OPENSSL_ROOT)/ssl \
		$(OPENSSL_ROOT)/.native-installed

clean:
	$(MAKE) -C acl clean
	rm -f $(ACL_OPENSSL_STATIC_STAMP)
	$(MAKE) zlib-clean
	$(MAKE) openssl-clean
