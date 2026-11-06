# Recomp build for Windows (x86-64).
#
#   make smm2                      the whole pipeline for Super Mario Maker 2 v3.0.3 from your own dump:
#                                  set up the game files, recompile main + sdk to C, build, (then `make run-smm2`)
#   make GAME=smm2 <target>        one step: setup | gen | all | run | runtime-objs | clean-gen
#
# The shared runtime is runtime/ (kernel, IPC, services, NVN on Vulkan, the Windows platform layer) and the recompiler
# is tools/. A game is games/<game>/: game.mk (identity, modules, dump layout) and its own glue code.
# Everything made from the dump goes to private/ (game files), generated/ (recompiled C) and build/; git ignores
# them and scripts/check_no_proprietary.py blocks them. They may be symlinks to another disk.
#
# Native: MSYS2 UCRT64 + Vulkan. Cross (any host, with zig as the compiler: tools/fetch_zig.sh, or ZIG=<zig>):
#   make TARGET=windows-x86_64 GAME=smm2 all   (DEPS=<headers + link libraries>)
UNAME := $(shell uname -s)
comma := ,
TARGET ?=
PLATFORM := windows
ARCH     := x86_64
ifneq ($(TARGET),)
ifneq ($(TARGET),windows-x86_64)
$(error this tree builds for Windows only: TARGET=windows-x86_64)
endif
endif

GAME     ?= smm2
GAMEDIR  := games/$(GAME)
GAME_MOVIES := 1  # (runtime/hle/movie.c + FFmpeg: games whose code uses the movie library; game.mk may say 0)
include $(GAMEDIR)/game.mk

# Game files (set up by `make setup` from the dump), recompiled code, build products: one folder each per game.
DATA     ?= private/$(GAME)
GAMEFILES := $(DATA)/extracted
GEN      ?= generated/$(GAME)-c
# OUT: holds the launcher exe and one folder, $(OUT)/$(DX_DIR): the runtime exe, its DLLs, the import library, python/, zig/,
# hactool/, tools/ (what it looks for), its saves and settings at run time, and work/ (objects, the fetched compiler and libraries)
DX_DIR   := smm2-dx_data
OUT      ?= ../build
B        ?= $(OUT)/$(DX_DIR)/work

# The guest window's base (runtime/rt.h GUEST_ORIGIN): the game code is generated for it.
GUEST_ORIGIN ?= 0x7000000000

ifneq ($(TARGET),)
# zig cc / c++ for the target (the generated code's FP/SIMD lowering is the portable C one on x86-64)
ZIG      ?= $(firstword $(wildcard ../build/smm2-dx_data/work/zig/zig) zig)
ZTARGET  := x86_64-windows-gnu
CROSS_CC := $(ZIG) cc -target $(ZTARGET)
CROSS_CXX := $(ZIG) c++ -target $(ZTARGET)
B        := $(OUT)/$(DX_DIR)/work
endif
GENSIMD  := --simd c

ifneq ($(TARGET),)
DEPS      ?= ../build/smm2-dx_data/work/deps-windows-x86_64
# a clone needs nothing fetched by hand: `make all` / `make package` get the compiler and the libraries first
# (tools/fetch_zig.sh, tools/fetch_deps.sh: pinned versions, sha256-checked, into ../build/smm2-dx_data/work)
ifneq ($(filter all package,$(or $(MAKECMDGOALS),all)),)
$(if $(wildcard ../build/smm2-dx_data/work/zig/lib),,$(shell tools/fetch_zig.sh >&2))
$(if $(wildcard $(DEPS)/include),,$(shell tools/fetch_deps.sh $(TARGET) $(DEPS) >&2))
endif
else
DEPS      ?=
endif
CC       := $(or $(CROSS_CC),clang)
CXX      := $(or $(CROSS_CXX),clang++)
LLD      := $(if $(CROSS_CC),,-fuse-ld=lld)  # (zig links with its own lld)
PLATFORM_SRC := runtime/platform/windows.c
IMGUI    := third_party/imgui
GENFLAGS := -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime
GAMEDEFS := -DGAME_TITLE_ID=$(GAME_TITLE_ID)ull -DGAME_ID='"$(GAME_ID)"' -DGAME_TITLE='"$(GAME_TITLE)"' -DGAME_BUNDLE_ID='"$(GAME_BUNDLE_ID)"'             -DGAME_DISPLAY_VERSION='"$(GAME_DISPLAY_VERSION)"'

ifneq ($(GUEST_ORIGIN),0x7000000000)
GEN      := $(GEN)-$(GUEST_ORIGIN)
B        := $(B)-$(GUEST_ORIGIN)
GAMEDEFS += -DGUEST_ORIGIN=$(GUEST_ORIGIN)ull
endif
GEN_SRC  := $(wildcard $(GEN)/main/*.c) $(wildcard $(GEN)/sdk/*.c)
GEN_OBJ  := $(patsubst %.c,$(B)/obj/%.o,$(patsubst $(GEN)/%,%,$(GEN_SRC))) $(B)/obj/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) \
            $(wildcard runtime/nvn/*.c) $(wildcard runtime/nvn/maxwell/*.c) $(PLATFORM_SRC) \
            $(wildcard runtime/net/*.c) \
            $(GAME_SRC)
ifeq ($(PLATFORM),windows)
RT_SRC   += runtime/platform/windows_exports.c  # (what the game code DLL calls in the exe)
endif
ifeq ($(strip $(GAME_MOVIES)),0)
RT_SRC   := $(filter-out runtime/hle/movie.c,$(RT_SRC))
FFMPEG_LIBS :=
else
FFMPEG_LIBS := -lavformat -lavcodec -lavutil -lswscale
endif
RT_HDRS  := $(wildcard runtime/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/nvn/*.h) \
            $(wildcard runtime/nvn/maxwell/*.h) $(wildcard runtime/nvn/vulkan/*.h) $(wildcard runtime/ui/*.h) \
            $(wildcard runtime/net/*.h) \
            $(GAMEDIR)/game.mk
UI_SRC   := $(wildcard runtime/ui/*.cpp)
UI_OBJ   := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(UI_SRC))
IMGUI_OBJ := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
             $(B)/imgui/imgui_impl_sdl3.o

EXE      := $(OUT)/$(DX_DIR)/$(GAME_ID)-dx-runtime.exe
# the exe players start: a small launcher that runs the runtime in $(DX_DIR) (runtime/platform/launcher_stub.c)
STUB     := $(OUT)/$(GAME_ID)-dx-$(PLATFORM)-$(ARCH).exe
IMPLIB   := $(OUT)/$(DX_DIR)/$(GAME_ID)-dx.dll.a
RT_DLLS  := $(if $(DEPS),$(addprefix $(OUT)/$(DX_DIR)/,SDL3.dll libiconv-2.dll libshaderc_shared.dll))
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -D_CRT_RAND_S -DWIN32_LEAN_AND_MEAN -DNOMINMAX
GENARCH  ?= $(if $(CROSS_CC),-mcpu=x86_64_v2,-march=x86-64-v2)  # (baseline; gamebuild.c upgrades to v3 where available)
GENFLAGS += -D_FILE_OFFSET_BITS=64 $(GENARCH)
LDFLAGS  := $(LLD) -Wl,--stack,16777216 $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 $(FFMPEG_LIBS) \
            -lwinmm -lole32 -lshell32 -lcomdlg32 -luuid -lshaderc_shared -lws2_32 -lbcrypt
RES_OBJ  := $(B)/windows.res  # (the exe's icon: runtime/platform/windows.rc)
# (vulkan-1.dll is looked up at run time, not imported: without it, the exe still starts and says what to install -
# tools/gen_vk_shim.py, runtime/platform/windows.c)
RES_OBJ  += $(B)/rt/vk_shim.o
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++ $(OSFLAGS)
GAMELIB  := $(OUT)/$(DX_DIR)/$(GAME_ID)game.dll
LDFLAGS  += -Wl,--export-all-symbols -Wl,--out-implib,$(IMPLIB)
GAMELIB_LDFLAGS := -shared $(LLD) $(IMPLIB)
GAMELIB_SHARED := -shared

# GAMELIB_ID: what the game code library is built from (the generator, the headers it is compiled against, the
# flags). A library with another id is never loaded.
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py \
                tools/simd_c.py tools/gamelib_entry.c $(GAME_HLE) $(wildcard $(GAME_PATCHES)) $(GAME_PATCH_DEPS) \
                runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS)') | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)
# The generated C must come from the same generator and patches: `make gen` stamps $(GEN)/.gen-<GAMELIB_ID>. Built from
# older generated code, a library carried the new id without the new patches (the iPad's game code was from
# 2026-10-18: no OpenCourse World handshake, no relaxed checks), so a stale tree stops the build instead.
ifneq ($(strip $(GEN_SRC)),)
ifeq ($(wildcard $(GEN)/.gen-$(GAMELIB_ID)),)
ifneq ($(filter all app,$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)),)  # (the goals that compile the game code)
$(error the generated game code in $(GEN) is older than the generator and patches (GAMELIB_ID $(GAMELIB_ID)): run `make GAME=$(GAME) gen` first)
endif
endif
endif

RT_OBJ   := $(patsubst %.c,$(B)/rt/%.o,$(patsubst runtime/%,%,$(RT_SRC)))
# TLS for the game's own ssl service (runtime/services/ssl.c): Mbed TLS 3.6 LTS, third_party/mbedtls (Apache-2.0),
# built with runtime/net/mbedtls_user_config.h (a TLS 1.2 client)
MBEDTLS  := third_party/mbedtls
MBEDTLS_FLAGS := -I$(MBEDTLS)/include -Iruntime/net -DMBEDTLS_USER_CONFIG_FILE='"mbedtls_user_config.h"'
MBEDTLS_OBJ := $(patsubst $(MBEDTLS)/library/%.c,$(B)/mbedtls/%.o,$(wildcard $(MBEDTLS)/library/*.c))
$(B)/rt/gamebuild.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"' -DGAMELIB_CFLAGS='"$(strip $(filter-out -Iruntime,$(GENFLAGS)))"' \
                                -DGAMELIB_SHARED='"$(GAMELIB_SHARED)"' -DGAMEBUILD_HLE='"$(GAME_HLE)"' \
                                -DGAMEBUILD_PATCHES='"$(GAME_PATCHES)"'

$(B)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

$(B)/mbedtls/%.o: $(MBEDTLS)/library/%.c runtime/net/mbedtls_user_config.h
	@mkdir -p $(dir $@)
	@$(CC) -O2 -g $(MBEDTLS_FLAGS) $(filter -D%,$(OSFLAGS)) -c $< -o $@  
$(B)/rt/services/ssl.o: RTFLAGS += $(MBEDTLS_FLAGS)
$(B)/rt/services/ssl.o: runtime/net/mbedtls_user_config.h
$(B)/rt/net/http.o: RTFLAGS += $(MBEDTLS_FLAGS)
$(B)/rt/net/http.o: runtime/net/mbedtls_user_config.h
$(B)/rt/net/roots.o: RTFLAGS += $(MBEDTLS_FLAGS)

# a game's own glue (games/<game>/*.c), compiled like the runtime; its own headers too (a struct that grows in one
# must rebuild every file that includes it)
GAME_HDRS := $(shell find $(GAMEDIR) -name '*.h' -not -path '*/generated/*' 2>/dev/null)
$(B)/rt/games/%.o: games/%.c $(RT_HDRS) $(GAME_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

# SMAA (third_party/smaa, MIT): the reference shader as a C string for runtime/nvn/smaa_data.c
$(B)/inc/smaa_hlsl.inc: third_party/smaa/SMAA.hlsl
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | LC_ALL=C tr '\200-\377' '?' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@
$(B)/rt/nvn/smaa_data.o: $(B)/inc/smaa_hlsl.inc
$(B)/rt/nvn/smaa_data.o: RTFLAGS += -I$(B)/inc -Ithird_party/smaa/Textures

$(B)/rt/%.o: runtime/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OSFLAGS) -c $< -o $@

# linked to a temp file, then renamed over: a copy still running keeps its (old) file
$(EXE): $(RT_OBJ) $(MBEDTLS_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(IMGUI_OBJ) $(EXE_GAME_OBJ) $(RES_OBJ)
	@mkdir -p $(B)/link $(OUT)/$(DX_DIR)
	$(CXX) -o $(B)/link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(B)/link/$(notdir $@) $@
	$(if $(and $(CROSS_CC),$(filter windows,$(PLATFORM))),(echo LIBRARY $(notdir $@); cat runtime/platform/windows_exports.def) > $(B)/exports.def && \
	    $(ZIG) dlltool -m i386:x86-64 -d $(B)/exports.def -l $(IMPLIB))

$(STUB): runtime/platform/launcher_stub.c $(B)/windows.res
	@mkdir -p $(OUT)
	$(CC) -O2 -s -Wl,--subsystem,windows -o $@ $^

# the runtime DLLs the exe needs beside it (from the fetched deps)
$(OUT)/$(DX_DIR)/%.dll: $(DEPS)/bin/%.dll
	@mkdir -p $(OUT)/$(DX_DIR)
	cp $< $@

# Windows: the Vulkan loader's entry points, forwarded to vulkan-1.dll loaded at run time (its export list:
# runtime/platform/vulkan-1.exports; the prototypes from the Vulkan headers)
$(B)/vk_shim.c: tools/gen_vk_shim.py runtime/platform/vulkan-1.exports
	@mkdir -p $(dir $@)
	$(or $(PY),python3) tools/gen_vk_shim.py $(if $(DEPS),$(DEPS)/include,/usr/include) runtime/platform/vulkan-1.exports $@
$(B)/rt/vk_shim.o: $(B)/vk_shim.c
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -Wno-unused-parameter -c $< -o $@

# Windows: the app icon as a resource (branding/app-icon.ico), compiled by zig's rc
$(B)/windows.res: runtime/platform/windows.rc branding/app-icon.ico
	@mkdir -p $(dir $@)
	$(ZIG) rc -fo $@ -i branding $<

# its id file (<library>.id) says what it was built from without loading it (runtime/gamelib.c)
$(GAMELIB): $(GEN_OBJ) | $(EXE)
	$(CC) $(GAMELIB_LDFLAGS) -o $@.tmp $^
	mv -f $@.tmp $@
	echo $(GAMELIB_ID) > $@.id

$(B)/rt/ui/%.o: runtime/ui/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UIFLAGS) -c $< -o $@

# The release this build is (the setup screen shows it; its update check compares it with GitHub's latest release):
# the tag, e.g. v0.1.0 (release builds pass RECOMP_VERSION=<tag>), else git's description, else "dev"
RECOMP_VERSION ?= $(shell git describe --tags --match 'v[0-9]*' 2>/dev/null || echo dev)
$(B)/rt/ui/launcher.o: CXXFLAGS += -DRECOMP_VERSION='"$(RECOMP_VERSION)"'
$(B)/rt/ui/menu.o: CXXFLAGS += -DRECOMP_VERSION='"$(RECOMP_VERSION)"'  # (the menu's About row)
$(B)/rt/ui/launcher.o: runtime/gamebuild.h runtime/paths.h

$(B)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_%.o: $(IMGUI)/backends/imgui_impl_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@


# ------------------------------------------------------------------ the pipeline
# Python for the recompiler: tools/venv (made by `make venv`: capstone, unicorn, lz4)
PY := $(firstword $(wildcard tools/venv/bin/python) $(wildcard tools/venv/Scripts/python.exe))
venv:
	@[ -n "$(PY)" ] || python3 -m venv tools/venv
	tools/venv/bin/pip install -q lz4 capstone unicorn

# Game files from the dump: games/<game>/setup.py checks them and lays them out in $(GAMEFILES) (exefs/ and romfs/;
# an update's romfs over the base game's, as links). Paths come from DUMP_* (game.mk defaults; override on the
# command line).
setup:
	$(if $(PY),,$(error run `make venv` first))
	$(PY) $(GAMEDIR)/setup.py $(SETUP_ARGS) --out $(GAMEFILES)

# Recompile every module to C (AArch64 -> C, on this machine, from the player's own files).
gen:
	$(if $(PY),,$(error run `make venv` first))
	cd tools && ../$(PY) gen.py sdk ../$(GAMEFILES)/exefs/sdk $(shell printf '%#x' $$(($(GUEST_ORIGIN) + 0x4000000))) ../$(GEN)/sdk --hle ../$(GAME_HLE) $(GENSIMD)
	cd tools && ../$(PY) gen.py main ../$(GAMEFILES)/exefs/main $(GUEST_ORIGIN) ../$(GEN)/main \
	    $(if $(wildcard $(GAME_PATCHES)),--patches ../$(GAME_PATCHES)) $(GENSIMD)
	rm -f $(GEN)/.gen-* && touch $(GEN)/.gen-$(GAMELIB_ID)

clean-gen:
	rm -rf $(GEN) $(B)/obj

# The player package (runtime with its setup screen, Python, recompiler, zig, hactool; nothing from the
# game): dist/SMM2Recomp-windows-x86_64.zip
#   make TARGET=windows-x86_64 GAME=smm2 DEPS=<deps> package
package: $(EXE)
	$(if $(TARGET),,$(error package: pass TARGET=windows-x86_64))
	RECOMP_VERSION=$(RECOMP_VERSION) tools/package.sh $(TARGET) $(OUT) $(GAME_ID) $(DEPS)

# Run from the data folder (saves, settings and shader caches go there); RUN_ARGS / env vars as the runtime reads them
run: all
	cd $(DATA) && $(CURDIR)/$(EXE) extracted $(RUN_ARGS)

gen-objs: $(GEN_OBJ)

# Runtime objects only (no generated code, no link): quick compile check, needs no game files
runtime-objs: $(RT_OBJ) $(MBEDTLS_OBJ) $(BACKEND_OBJ) $(UI_OBJ)

# Recompiler unit tests (no game files)
test-tools:
	cd tools && ../$(PY) test_lift.py

# One command per game: `make smm2` = setup + gen + build. (Then `make run-smm2`.)
smm2:
	$(MAKE) GAME=smm2 setup
	$(MAKE) GAME=smm2 gen
	$(MAKE) GAME=smm2 -j$(JOBS) all
run-smm2:
	$(MAKE) GAME=smm2 run
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# Shader prefetch's build states / waits / pending table (runtime/nvn/buildsync.c), no game files
../build/smm2-dx_data/work/test_buildsync: tools/test_buildsync.c runtime/nvn/buildsync.c runtime/nvn/buildsync.h
	@mkdir -p ../build/work
	$(HOSTCC_TEST) -O1 -Wall -pthread '-DLOG(...)=printf(__VA_ARGS__)' -o $@ tools/test_buildsync.c runtime/nvn/buildsync.c
test-buildsync: ../build/smm2-dx_data/work/test_buildsync
	../build/smm2-dx_data/work/test_buildsync
HOSTCC_TEST ?= cc

# Frame generation (runtime/nvn/framegen*): the per-frame decision, no game files
../build/smm2-dx_data/work/test_framegen_logic: tools/test_framegen_logic.c runtime/nvn/framegen.c runtime/nvn/framegen.h
	mkdir -p ../build/work
	clang -O1 -DFG_NO_SDL -I runtime -o $@ tools/test_framegen_logic.c runtime/nvn/framegen.c
test-framegen: ../build/smm2-dx_data/work/test_framegen_logic
	../build/smm2-dx_data/work/test_framegen_logic

.PHONY: all venv setup gen clean-gen run gen-objs runtime-objs test-tools test-buildsync test-framegen smm2 run-smm2 package embed-server
