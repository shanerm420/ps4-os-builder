# PS4 Linux - PKG builds (OpenOrbis).
#
# Two packages, one source tree:
#   make APP=install   -> PS4LNX001  PS4 XFCE Linux: installs, starts, repairs
#                                  (installer/main.c)
#   make APP=remove    -> PS4LNX002  the ChromeOS remover (installer/remove.c)
#
# Separate apps on purpose: the only code that removes a user's files lives in the
# package whose entire job is deleting. The installer contains no deletion of
# pre-existing files, so "install Linux" can never erase a ChromeOS install.
#
# Build them one at a time, cleaning in between: the intermediate names
# (eboot.bin, pkg.gp4, sce_sys/param.sfo) are shared.
#
# Mirrors the reference project's proven recipe, including the one non-obvious
# step: create-gp4 only declares assets/{audio,fonts,images,misc,videos}, so the
# assets/ps4linux directory has to be injected into the .gp4 afterwards. Skip
# that and packaging "succeeds" while the app cannot find its files at runtime.
#
# Requires OO_PS4_TOOLCHAIN to point at a *built* OpenOrbis toolchain
# (bin/linux/{PkgTool.Core,create-gp4,create-fself} present). The git repository
# alone is not enough - use a release build.
APP         ?= install

ifeq ($(APP),remove)
TITLE       := PS4 Linux: Remove ChromeOS
VERSION     := 0.1
TITLE_ID    := PS4LNX002
CONTENT_ID  := IV0000-PS4LNX002_00-PS4LINUXREMOVER0
CFILES      := installer/remove.c $(UI_SRC)
ASSETS      :=
else
TITLE       := PS4 XFCE Linux
VERSION     := 0.1
TITLE_ID    := PS4LNX001
CONTENT_ID  := IV0000-PS4LNX001_00-PS4LINUXDESKTOP0
CFILES      := installer/main.c $(UI_SRC)
# rootfs.tar.zst is optional: only shipped when WITH_ROOTFS=1 (on-console populate).
ASSETS      := assets/ps4linux/bzImage assets/ps4linux/initramfs.cpio.gz \
               assets/ps4linux/ps4linux-loader.bin assets/ps4linux/build-id \
               $(wildcard assets/ps4linux/rootfs.tar.zst)
endif

# The screens are drawn on the CPU with stb_truetype, so both apps ship the fonts
# they draw with - the remover draws its confirmation screen too. create-gp4
# already declares assets/fonts.
FONT_ASSETS := assets/fonts/DejaVuSans.ttf assets/fonts/DejaVuSans-Bold.ttf \
               assets/fonts/DejaVuSansMono.ttf
ASSETS      += $(FONT_ASSETS)

LIBS        := -lc -lkernel -lSceMsgDialog -lSceCommonDialog -lSceSysmodule \
               -lSceSystemService -lSceNet \
               -lSDL2 -lSceVideoOut -lSceAudioOut -lScePad -lSceUserService \
               -lSceFreeType

TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
PROJDIR     := installer
INTDIR      := build
UI_SRC      := ui/ui.c ui/status.c ui/soft.c ui/screen.c ui/appui.c
UI_OBJS     := $(addprefix $(INTDIR)/, ui.o status.o soft.o screen.o appui.o)
OBJS        := $(patsubst $(PROJDIR)/%.c, $(INTDIR)/%.o, $(CFILES)) $(UI_OBJS)
CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wextra \
               -Wno-unused-parameter -c -isysroot $(TOOLCHAIN) \
               -isystem $(TOOLCHAIN)/include -Iui
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr \
               -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o
CC          := clang
LD          := ld.lld
PKGTOOL     := $(TOOLCHAIN)/bin/linux/PkgTool.Core

_unused     := $(shell mkdir -p $(INTDIR))

all: check-ids check-stb $(CONTENT_ID).pkg

# LibOrbisPkg refuses anything but exactly 36 characters (IV0000- + 9-char
# TITLE_ID + _00- + 16 chars). It throws a .NET stack trace, so check first.
# The stb headers are third-party and not committed: scripts/get-stb.sh fetches
# them. Fail with that instruction instead of a confusing include error.
check-stb:
	@[ -f ui/stb_truetype.h ] || { \
		echo "ui/stb_truetype.h is missing - run scripts/get-stb.sh" >&2; exit 1; }

check-ids:
	@n=$$(printf %s '$(CONTENT_ID)' | wc -c); \
	[ "$$n" = 36 ] || { \
		echo "CONTENT_ID must be 36 characters, is $$n: '$(CONTENT_ID)'" >&2; exit 1; }
	@t=$$(printf %s '$(TITLE_ID)' | wc -c); \
	[ "$$t" = 9 ] || { echo "TITLE_ID must be 9 characters, is $$t" >&2; exit 1; }

$(CONTENT_ID).pkg: pkg.gp4
	$(PKGTOOL) pkg_build $< .

# sce_module/: the OpenOrbis libc runtime. The app does not start without it.
# These are NOT in this repository - the toolchain ships them (bin/data/modules on
# release builds, src/modules on the v0.5.4 native SDK). A missing rule here used
# to be a "No rule to make target" failure with no explanation.
LIBMODULES  := sce_module/libc.prx sce_module/libSceFios2.prx
MODSRC      := $(firstword $(wildcard $(TOOLCHAIN)/bin/data/modules) $(wildcard $(TOOLCHAIN)/src/modules))

sce_module/%.prx:
	@[ -n "$(MODSRC)" ] || { \
		echo "no runtime modules under $(TOOLCHAIN)/bin/data/modules or $(TOOLCHAIN)/src/modules" >&2; \
		echo "get a release build of the OpenOrbis toolchain, not the git repository" >&2; exit 1; }
	@mkdir -p sce_module
	cp "$(MODSRC)/$*.prx" $@

# Package artwork. Both tiles are this project's own, 512x512 PNG - the size the
# home screen loads. The installer gets the console mark; the remover gets the same
# mark dimmed and struck through, so the two are told apart at a glance on the XMB.
# The OpenOrbis sample is only a fallback for a tree where the file is missing.
ifeq ($(APP),remove)
ARTWORK     := artwork/icon0-remove.png
else
ARTWORK     := artwork/icon0.png
endif
SAMPLES     := $(TOOLCHAIN)/samples/hello_world/sce_sys

sce_sys/about/right.sprx:
	@[ -f "$(SAMPLES)/about/right.sprx" ] || { \
		echo "no package artwork at $(SAMPLES)/about/right.sprx" >&2; \
		echo "set OO_PS4_TOOLCHAIN to a full toolchain (samples/ must exist)" >&2; exit 1; }
	@mkdir -p sce_sys/about
	cp "$(SAMPLES)/about/right.sprx" $@

# FORCE: an icon left over from an earlier build would otherwise be kept, since
# the target has no prerequisites that change when the artwork does.
sce_sys/icon0.png: FORCE
	@mkdir -p sce_sys
	@if [ -f "$(ARTWORK)" ]; then \
		echo "  icon: $(ARTWORK) (this project's tile)"; \
		cp "$(ARTWORK)" $@; \
	elif [ -f "$(SAMPLES)/icon0.png" ]; then \
		echo "  icon: OpenOrbis sample - $(ARTWORK) is missing from this tree"; \
		cp "$(SAMPLES)/icon0.png" $@; \
	else \
		echo "no package artwork: neither $(ARTWORK) nor $(SAMPLES)/icon0.png" >&2; \
		echo "set OO_PS4_TOOLCHAIN to a full toolchain (samples/ must exist)" >&2; \
		exit 1; \
	fi

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png \
         $(LIBMODULES) $(ASSETS)
	$(TOOLCHAIN)/bin/linux/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"
ifeq ($(APP),install)
	# create-gp4 only declares assets/{audio,fonts,images,misc,videos}; PkgTool needs assets/ps4linux too.
	sed -i 's|<dir targ_name="misc" />|<dir targ_name="misc" />\n\t\t\t<dir targ_name="ps4linux" />|' $@
	grep -q '<dir targ_name="ps4linux" />' $@
endif

# Always regenerate: this file carries TITLE and TITLE_ID, which differ per APP.
# Depending only on the Makefile let a remover param.sfo survive into the
# installer package - header PS4LNX001, title inside "PS4 Linux: Remove ChromeOS".
sce_sys/param.sfo: Makefile FORCE
	$(PKGTOOL) sfo_new $@
	$(PKGTOOL) sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(PKGTOOL) sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(PKGTOOL) sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 2
	$(PKGTOOL) sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(PKGTOOL) sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(PKGTOOL) sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(PKGTOOL) sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(PKGTOOL) sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(OBJS)
	$(LD) $(OBJS) -o $(INTDIR)/$(APP).elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/linux/create-fself -in=$(INTDIR)/$(APP).elf \
		-out=$(INTDIR)/$(APP).oelf --eboot "eboot.bin" --paid 0x3800000000000011

$(INTDIR)/%.o: $(PROJDIR)/%.c
	$(CC) $(CFLAGS) -o $@ $<

# The UI sources live beside installer/, so they need their own rule. They are
# shared by both packages and by tools/ui-preview.
$(INTDIR)/%.o: ui/%.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f *.pkg pkg.gp4 sce_sys/param.sfo eboot.bin $(INTDIR)/*.elf \
		$(INTDIR)/*.oelf $(INTDIR)/*.o

FORCE:

.PHONY: all clean check-ids FORCE
