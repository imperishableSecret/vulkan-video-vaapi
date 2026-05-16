BUILDDIR ?= build
DESTDIR ?=

MESON ?= meson
MESON_COMPILE ?= $(MESON) compile
MESON_INSTALL ?= $(MESON) install
MESON_TEST ?= $(MESON) test
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy
JQ ?= jq

OPTIMIZATION ?= 2
DEBUG ?= false
NDEBUG ?= true
MESON_SETUP_OPTIONS ?= -Doptimization=$(OPTIMIZATION) -Ddebug=$(DEBUG) -Db_ndebug=$(NDEBUG) -Dcpp_args= -Dcpp_link_args=
EXPERIMENTAL_MESON_SETUP_OPTIONS ?= -Doptimization=3 -Ddebug=false -Db_ndebug=true -Dcpp_args=-march=native -Dcpp_link_args=-march=native

FORMAT_FILES := $(shell find src tests -type f \( -name '*.cpp' -o -name '*.h' \) | sort)
TIDY_FILES := $(shell find src tests -type f -name '*.cpp' | sort)
TIDY_BUILDDIR := $(BUILDDIR)/clang-tidy

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

install:
	@test -f "$(BUILDDIR)/build.ninja" || { \
		echo "missing $(BUILDDIR)/build.ninja; run 'make all' or 'make experimental' first" >&2; \
		exit 1; \
	}
	$(MESON_INSTALL) -C "$(BUILDDIR)" --no-rebuild $(if $(DESTDIR),--destdir "$(DESTDIR)")

format-fix:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

tidy: all
	@mkdir -p "$(TIDY_BUILDDIR)"
	$(JQ) 'unique_by(.file) | map(.command |= gsub(" -fno-gnu-unique"; ""))' "$(BUILDDIR)/compile_commands.json" > "$(TIDY_BUILDDIR)/compile_commands.json"
	$(CLANG_TIDY) --quiet -p "$(TIDY_BUILDDIR)" $(TIDY_FILES)

clean:
	@if [ -f "$(BUILDDIR)/build.ninja" ]; then \
		$(MESON_COMPILE) -C "$(BUILDDIR)" --clean; \
	fi
