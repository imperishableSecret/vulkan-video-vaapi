BUILDDIR ?= build
DESTDIR ?=
PREFIX ?= $(HOME)/.local
LIBVA_DRIVER_DIR ?= $(PREFIX)/lib/dri

MESON ?= meson
MESON_COMPILE ?= $(MESON) compile
MESON_TEST ?= $(MESON) test
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy
INSTALL ?= install

OPTIMIZATION ?= 2
DEBUG ?= false
NDEBUG ?= true
MESON_SETUP_OPTIONS ?= -Doptimization=$(OPTIMIZATION) -Ddebug=$(DEBUG) -Db_ndebug=$(NDEBUG) -Dcpp_args= -Dcpp_link_args=
EXPERIMENTAL_MESON_SETUP_OPTIONS ?= -Doptimization=3 -Ddebug=false -Db_ndebug=true -Dcpp_args=-march=native -Dcpp_link_args=-march=native

FORMAT_FILES := $(shell find src tests -type f \( -name '*.cpp' -o -name '*.h' \) | sort)
TIDY_FILES := $(shell find src tests -type f -name '*.cpp' | sort)

.PHONY: all setup experimental test install format-fix tidy clean

all: setup
	$(MESON_COMPILE) -C $(BUILDDIR)

setup:
	@if [ -f "$(BUILDDIR)/build.ninja" ]; then \
		$(MESON) setup "$(BUILDDIR)" --reconfigure $(MESON_SETUP_OPTIONS); \
	else \
		$(MESON) setup "$(BUILDDIR)" $(MESON_SETUP_OPTIONS); \
	fi

experimental:
	@if [ -f "$(BUILDDIR)/build.ninja" ]; then \
		$(MESON) setup "$(BUILDDIR)" --reconfigure $(EXPERIMENTAL_MESON_SETUP_OPTIONS); \
	else \
		$(MESON) setup "$(BUILDDIR)" $(EXPERIMENTAL_MESON_SETUP_OPTIONS); \
	fi
	$(MESON_COMPILE) -C $(BUILDDIR)

test: all
	$(MESON_TEST) -C $(BUILDDIR) --print-errorlogs

install: all
	$(INSTALL) -Dm755 "$(BUILDDIR)/nvidia_vulkan_drv_video.so" "$(DESTDIR)$(LIBVA_DRIVER_DIR)/nvidia_vulkan_drv_video.so"

format-fix:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

tidy: all
	$(CLANG_TIDY) -p "$(BUILDDIR)" $(TIDY_FILES)

clean:
	@if [ -f "$(BUILDDIR)/build.ninja" ]; then \
		$(MESON_COMPILE) -C "$(BUILDDIR)" --clean; \
	fi
