# libjzintv_core: the toolkit-agnostic, SDL-free jzIntv core.
#
# jzIntv selects its graphics/sound/event/joystick/platform backend entirely
# by *which files* provide gfx_init/snd_init/event_plat_tick/plat_init/main --
# never by #ifdef inside a shared file (see cmake/StageJzIntv.cmake's header
# comment). Upstream already ships and builds a "null" backend combination for
# exactly this purpose -- gfx_null.c, snd_null.c, event_null.c, joy_null.c,
# plat_null.c, main_null.c, wired together as $(OBJS) + $(OBJS_NULL) and built
# as PROG_NULL / libjzintv_null.a (see src/Makefile, src/Makefile.common) --
# and jzintv.c (the non-emscripten entry point) never references SDL at the
# source level, so it needs no changes at all to run against that backend.
#
# This target is exactly that combination, minus main_null.c's main() (see
# USE_AS_BACKEND below -- the Wii port uses the same flag to link jzIntv as a
# library called from someone else's main loop, which is our situation too).
#
# core/jzintv/intv_host.c is the thing that calls jzintv_entry_point() on its
# own thread; the null gfx/snd backends will be replaced in a later milestone
# by our own frame/audio-capturing desktop/*.c files -- for now this proves
# the core builds and links standalone, with zero SDL symbols.

set(JZ "${CMAKE_SOURCE_DIR}/core/jzintv-generated")

if(NOT EXISTS "${JZ}/src/jzintv.c")
  message(FATAL_ERROR
    "Staged jzIntv tree is missing. cmake/StageJzIntv.cmake should have "
    "produced it; re-run with -DJZINTV_RESTAGE=ON.")
endif()

# The Locutus cartridge-format decoder (src/locutus/) is C++14 -- the one
# piece of jzIntv that isn't C, per its own subMakefile ($(CXX) -std=c++14).
# Everything else here is C; enabling C++ only in this subdirectory keeps a
# core-only configure from requiring a C++ compiler for anything else.
enable_language(CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---- the staged jzIntv core, $(OBJS) from src/*/subMakefile --------------
set(JZINTV_CORE_SOURCES
  src/svn_revision.c
  src/avi/avi.c
  src/event/event.c
  src/event/event_tbl.c
  src/mem/mem.c
  src/cfg/cfg.c
  src/cfg/usage.c
  src/cfg/mapping.c
  src/file/file.c
  src/file/elfi.c
  src/fujinet/fujibus.c
  src/fujinet/fn_sock.c
  src/fujinet/fujinet.c
  src/fujinet/fujinet_config_rom.c
  src/metadata/metadata.c
  src/metadata/cfgvar_metadata.c
  src/metadata/icarttag_metadata.c
  src/metadata/print_metadata.c
  src/gif/gif_enc.c
  src/gif/lzw_enc.c
  src/gfx/palette.c
  src/gfx/gfx.c
  src/speed/speed.c
  src/cp1600/cp1600.c
  src/cp1600/op_decode.c
  src/cp1600/op_exec.c
  src/cp1600/emu_link.c
  src/cp1600/op_exec_ext.c
  src/cp1600/req_q.c
  src/cp1600/tbl/fn_cond_br.c
  src/cp1600/tbl/fn_dir_2op.c
  src/cp1600/tbl/fn_imm_2op.c
  src/cp1600/tbl/fn_impl_1op_a.c
  src/cp1600/tbl/fn_impl_1op_b.c
  src/cp1600/tbl/fn_ind_2op.c
  src/cp1600/tbl/fn_reg_1op.c
  src/cp1600/tbl/fn_reg_2op.c
  src/cp1600/tbl/fn_rot_1op.c
  src/cp1600/tbl/formats.c
  src/jlp/jlp.c
  src/periph/periph.c
  src/minilzo/minilzo.c
  src/zlib/deflate.c
  src/zlib/adler32.c
  src/zlib/trees.c
  src/zlib/zutil.c
  src/name/name.c
  src/name/name_list.c
  src/misc/crc32.c
  src/misc/crc16.c
  src/misc/avl.c
  src/misc/ll.c
  src/misc/file_crc32.c
  src/misc/types.c
  src/misc/jzprint.c
  src/misc/printer.c
  src/lzoe/lzoe.c
  src/icart/icart.c
  src/icart/icartrom.c
  src/icart/icartbin.c
  src/icart/icartfile.c
  src/icart/icarttag.c
  src/icart/icarttag_authors.c
  src/icart/icarttag_printer.c
  src/ivoice/ivoice.c
  src/locutus/locutus.cpp
  src/locutus/luigi.cpp
  src/locutus/locutus_adapt.cpp
  src/locutus/locutus_types.cpp
  src/locutus/loc_to_bin.cpp
  src/locutus/bin_to_loc.cpp
  src/locutus/rom_to_loc.cpp
  src/emscripten/web_files.c
  src/serializer/serializer.c
  src/bincfg/bincfg.c
  src/bincfg/bincfg_grmr.tab.c
  src/bincfg/bincfg_lex.c
  src/bincfg/legacy.c
  src/ecs/ecs.c
  src/ay8910/ay8910.c
  src/demo/demo.c
  src/debug/debug.c
  src/debug/debug_dasm1600.c
  src/util/symtab.c
  src/debug/source.c
  src/stic/stic.c
  src/pads/pads.c
  src/pads/pads_cgc.c
  src/cheat/cheat.c
  src/scale/scale2x.c
  src/scale/scale3x.c
  src/plat/plat_lib.c
  src/plat/gnu_getopt.c
  src/plat/plat_gen.c
  src/mvi/mvi.c
  # The non-emscripten entry point (jzintv_entry_point + the blocking
  # simulator loop). Verified SDL-free at the source level -- it only calls
  # through the gfx_t/snd_t/event_t/plat_* backend abstractions, never SDL
  # directly. src/jzintv_em.c (the emscripten split-loop variant) is NOT
  # compiled here; its callback-driven loop calls emscripten_set_main_loop()
  # and friends directly and has no desktop equivalent.
  src/jzintv.c
  # ---- the "null" backend combination (OBJS_NULL in src/*/subMakefile) ----
  # gfx_null.c and snd_null.c are replaced by our own gfx_desktop.c (frame
  # capture into core/jzintv/intv_frame.h's store) and snd_desktop.c (audio
  # capture into intv_audio.h's ring buffer); event/joy/plat stay the
  # upstream null drivers for now (real keyboard/gamepad event injection
  # lands in a later milestone -- see TODO; note this is distinct from the
  # direct pad injection intv_host.c already exposes, which is what the
  # keypad window and intvsession_key_from_keysym() both use today).
  src/event/event_null.c
  src/joy/joy_null.c
  src/plat/plat_null.c
  src/plat/main_null.c
)
list(TRANSFORM JZINTV_CORE_SOURCES PREPEND "${JZ}/")

list(APPEND JZINTV_CORE_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/jzintv/desktop/gfx_desktop.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/jzintv/desktop/snd_desktop.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/jzintv/intv_frame.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/jzintv/intv_audio.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/jzintv/intv_host.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/debugger/debugger.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/debugger/sticview.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/debugger/symbols.c"
)

add_library(jzintv_core STATIC ${JZINTV_CORE_SOURCES})
set_target_properties(jzintv_core PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(jzintv_core PUBLIC
  "${JZ}/src" "${CMAKE_CURRENT_SOURCE_DIR}/jzintv" "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(jzintv_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(jzintv_core PUBLIC
  JZINTV_VERSION_MAJOR=0x00
  JZINTV_VERSION_MINOR=0x01
  # main_null.c: skip its main() -- core/jzintv/intv_host.c calls
  # jzintv_entry_point() itself, on its own thread. Same flag the Wii port
  # uses to link jzIntv as a library (src/Makefile.wii).
  USE_AS_BACKEND
)
if(NOT WIN32)
  # src/config.h's PLAT_LINUX block sets CAN_TIOCGWINSZ (for
  # get_disp_width()'s terminal-width detection, used only by the console
  # debugger's line-wrapping) but -- unlike its PLAT_MACOS/__FreeBSD__
  # blocks -- never sets USE_SYS_IOCTL, so plat_lib.c's <sys/ioctl.h> include
  # is skipped and struct winsize/TIOCGWINSZ/ioctl() go undeclared. Apple's
  # and FreeBSD's libc pull sys/ioctl.h in transitively through <termios.h>;
  # glibc does not. This is a compensating compile definition, not a source
  # patch -- USE_SYS_IOCTL is an upstream-recognized macro, just never wired
  # to the Linux branch.
  target_compile_definitions(jzintv_core PRIVATE USE_SYS_IOCTL)
endif()
# jzIntv's C is permissive about these; upstream's to fix, not ours to warn on.
target_compile_options(jzintv_core PRIVATE
  -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable)
target_link_libraries(jzintv_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(jzintv_core PUBLIC ws2_32)
else()
  target_link_libraries(jzintv_core PUBLIC m)
endif()

# ---- the Intellivision system ROMs -----------------------------------------
# jzIntv resolves EXEC/GROM by exact path (its -e/-g flags), not by searching
# a ROM directory the way XRoar does -- so intv_host.c (M3) materialises
# whatever is embedded here into the session's ROM directory on first start
# and passes -e/-g pointing at it. What changes with WITH_INTV_ROMS is only
# whether this table has anything in it. See COMPLIANCE.md.
set(INTV_ROM_DIR_SRC "${CMAKE_SOURCE_DIR}/tools/jzintv/roms")
set(INTV_SYSTEM_ROMS
  "${INTV_ROM_DIR_SRC}/exec.bin"
  "${INTV_ROM_DIR_SRC}/grom.bin")

if(WITH_INTV_ROMS)
  set(_embed_roms ${INTV_SYSTEM_ROMS})
else()
  set(_embed_roms "")
  message(STATUS
    "WITH_INTV_ROMS=OFF: the Mattel EXEC/GROM are NOT embedded; the app "
    "reads them from the user's ROM directory at run time")
endif()

set(INTV_ROMS_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded.c")
add_custom_command(
  OUTPUT "${INTV_ROMS_C}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/tools/jzintv/embed-roms.py"
          "${INTV_ROMS_C}"
          --roms ${_embed_roms}
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/jzintv/embed-roms.py"
          ${_embed_roms}
  COMMENT "Embedding the Intellivision ROMs (WITH_INTV_ROMS=${WITH_INTV_ROMS})"
  VERBATIM)

target_sources(jzintv_core PRIVATE "${INTV_ROMS_C}")
target_include_directories(jzintv_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# ---- intv_session: the toolkit-agnostic session API (intvsession.h) -------
# Deliberately a SEPARATE target from jzintv_core, not more files bolted onto
# it: gamepad_sdl.c needs SDL3 (for gamepad enumeration/input only -- audio
# and video capture are jzintv_core's own gfx_desktop.c/snd_desktop.c and
# need no toolkit at all), and jzintv_core's whole point is staying
# SDL-free -- verified by no_sdl_link_test, which links jzintv_core alone.
# Were gamepad_sdl.c added directly to jzintv_core, every consumer of that
# archive would pick up an SDL3 runtime dependency whether or not it uses
# gamepads, silently widening what "zero SDL" meant. This mirrors
# fujinet-go-coco-desktop's own split (xroar_core vs. cocosession, the
# latter linking SDL3 for audio/gamepad).
find_package(SDL3 CONFIG REQUIRED)

add_library(intv_session STATIC
  src/settings.c
  src/paths.c
  src/session.c
  src/intv_keymap.c
  src/gamepad_sdl.c
  src/audio_sdl.c
  src/fujinet_runtime.c
)
set_target_properties(intv_session PROPERTIES C_STANDARD 11)
target_include_directories(intv_session PRIVATE src)
target_link_libraries(intv_session PUBLIC jzintv_core PRIVATE SDL3::SDL3)
target_compile_options(intv_session PRIVATE -Wall -Wextra -Wno-unused-parameter
                       -Wno-format-truncation)
# fujinet_runtime.c dlopen's libfujinet at run time -- no link-time
# dependency on it, so intv_session (and everything linking it) builds fine
# with -DWITH_FUJINET=OFF. These are where paths_provision_fujinet
# (paths.c) looks for the runtime and its data tree when nothing was found
# beside the executable or an explicit FUJINET_LIB override.
target_link_libraries(intv_session PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(intv_session PRIVATE
  INTV_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  INTV_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-intv"
  INTV_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-intv"
)

# ---- tests -----------------------------------------------------------------
# Link jzintv_core directly (not intv_session) wherever a test only needs
# intv_host/intv_frame/intv_audio -- keeps them exercising the exact
# SDL-free archive no_sdl_link_test checks, and is a second, incidental
# confirmation that gamepad support genuinely stayed opt-in.
add_executable(no_sdl_link_test tests/no_sdl_link_test.c)
target_link_libraries(no_sdl_link_test jzintv_core)

add_executable(boot_smoke tests/boot_smoke.c)
target_link_libraries(boot_smoke jzintv_core)
# roms_embedded.h (src/) so the test can check intv_embedded_rom_count itself.
target_include_directories(boot_smoke PRIVATE src)

add_executable(pad_test tests/pad_test.c)
target_link_libraries(pad_test jzintv_core)
target_include_directories(pad_test PRIVATE src)

add_executable(session_test tests/session_test.c)
target_link_libraries(session_test intv_session)
target_include_directories(session_test PRIVATE src)

add_executable(keymap_test tests/keymap_test.c)
target_link_libraries(keymap_test intv_session)

add_executable(audio_test tests/audio_test.c)
target_link_libraries(audio_test jzintv_core)
target_include_directories(audio_test PRIVATE src)

# Pure function test (no ROMs, no emulator boot) -- runs regardless of
# WITH_INTV_ROMS, unlike audio_test above.
add_executable(audio_ring_test tests/audio_ring_test.c)
target_link_libraries(audio_ring_test jzintv_core)

add_executable(gamepad_test tests/gamepad_test.c)
target_link_libraries(gamepad_test intv_session)
target_include_directories(gamepad_test PRIVATE src)

add_executable(fujibus_smoke tests/fujibus_smoke.c)
target_link_libraries(fujibus_smoke intv_session)

add_executable(debug_test tests/debug_test.c)
target_link_libraries(debug_test jzintv_core)
target_include_directories(debug_test PRIVATE src)

add_executable(stic_test tests/stic_test.c)
target_link_libraries(stic_test jzintv_core)
target_include_directories(stic_test PRIVATE src)

add_executable(symtab_test tests/symtab_test.c)
target_link_libraries(symtab_test jzintv_core)
target_include_directories(symtab_test PRIVATE debugger)

add_test(NAME no_sdl_link COMMAND no_sdl_link_test)
add_test(NAME boot_smoke COMMAND boot_smoke)
add_test(NAME pad_test COMMAND pad_test)
add_test(NAME session COMMAND session_test)
add_test(NAME keymap COMMAND keymap_test)
add_test(NAME audio COMMAND audio_test)
add_test(NAME audio_ring COMMAND audio_ring_test)
add_test(NAME gamepad COMMAND gamepad_test)
add_test(NAME fujibus_smoke COMMAND fujibus_smoke)
add_test(NAME debug COMMAND debug_test)
add_test(NAME stic COMMAND stic_test)
add_test(NAME symtab COMMAND symtab_test)
set_tests_properties(fujibus_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30
                     RUN_SERIAL ON)
set_tests_properties(debug stic PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
set_tests_properties(pad_test boot_smoke audio PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
# Needs WITH_INTV_ROMS=ON to have anything to provision; reported as 77
# (skipped) rather than a failure otherwise, and it boots a real machine, so
# it is the one test here that takes real wall-clock time.
set_tests_properties(boot_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
