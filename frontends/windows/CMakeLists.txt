# Windows frontend: native Win32 (GDI) over the shared session core. Built
# only on Windows hosts (or cross-compiled with cmake/toolchains/
# mingw-w64.cmake for a Wine smoke test -- see that file's own header).
# WIN32_EXECUTABLE => a WinMain entry point, no console window.

if(NOT WIN32)
  return()
endif()

enable_language(RC)

# The manifest carries the app version in its assembly identity, so it is
# generated from PROJECT_VERSION rather than repeating it. resource.rc picks
# the generated copy up through the RC compiler's include path.
configure_file(app.manifest.in "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" @ONLY)

add_executable(fujinet-go-intv-windows WIN32
  main.c
  key_translate.c
  debugger/dbg_window.c
  keypad/keypad_window.c
  ecskbd/ecskbd_window.c
  resource.rc
)
# See frontends/gnome/CMakeLists.txt's own comment on the underlying
# issue: jzintv_core has one C++ translation unit (src/locutv/), and this
# target's own sources are all .c, so CMake picks the plain C linker
# driver -- which never links libstdc++ at all (not even given
# -static-libstdc++ below, a g++-only flag the C driver ignores).
# Confirmed by an actual cross-build: naming jzintv_core directly (as
# GNOME's own CMakeLists.txt does, where it FIXES the problem) was not
# enough here, so link the C++ runtime explicitly instead, the same
# resolution frontends/gnome/CMakeLists.txt settled on.
target_link_libraries(fujinet-go-intv-windows PRIVATE
  intv_session jzintv_core stdc++
  gdi32 user32 comdlg32 shell32 comctl32
  winmm  # timeBeginPeriod, for frame pacing -- see main.c's WinMain
)
target_include_directories(fujinet-go-intv-windows PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_SOURCE_DIR}/core/debugger")
target_compile_options(fujinet-go-intv-windows PRIVATE -Wall -Wextra)

# Bundle the MinGW runtime statically so the .exe has no loose DLL deps
# beyond SDL3.dll itself (intv_session links SDL3 for gamepad/audio).
target_link_options(fujinet-go-intv-windows PRIVATE
  -static -static-libgcc -static-libstdc++)

# The VK -> keysym translation is the one part of this frontend worth
# testing without a human at a keyboard, and the one that most needs it: the
# maintainer has no Windows machine, so a gap in it is invisible until a user
# reports that some key will not map. Same shape as the KDE frontend's own
# keyforward_test -- a plain console executable over the split-out
# translation unit, no window and no session required.
add_executable(keytranslate_test keytranslate_test.c key_translate.c)
target_link_libraries(keytranslate_test PRIVATE intv_session user32)
target_include_directories(keytranslate_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(keytranslate_test PRIVATE -Wall -Wextra)
# Same static link as the app below: without it the .exe wants MinGW's
# libgcc/libstdc++/winpthread DLLs on PATH, which neither a Wine smoke run
# nor CI's ctest step provides.
target_link_options(keytranslate_test PRIVATE
  -static -static-libgcc -static-libstdc++)
add_test(NAME keytranslate COMMAND keytranslate_test)

install(TARGETS fujinet-go-intv-windows RUNTIME DESTINATION bin)
