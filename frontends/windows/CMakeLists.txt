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
  debugger/dbg_window.c
  keypad/keypad_window.c
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

install(TARGETS fujinet-go-intv-windows RUNTIME DESTINATION bin)
