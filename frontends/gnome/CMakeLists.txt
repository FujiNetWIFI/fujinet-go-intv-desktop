# GNOME frontend: GTK4 + libadwaita. Found-or-skipped so a lean machine can
# still build the core / other frontends.
#
# No embedded FujiNet web view yet (WITH_WEBVIEW / WebKitGTK land later,
# matching the sibling repos' own staging) -- FujiNet Configuration opens in
# the system browser for now.

find_package(PkgConfig REQUIRED)
pkg_check_modules(ADW IMPORTED_TARGET libadwaita-1>=1.4 gtk4>=4.10)

if(NOT ADW_FOUND)
  message(STATUS "GNOME frontend: libadwaita/gtk4 not found -- skipping")
  return()
endif()

set(GNOME_SOURCES
  main.c
  window.c
  display.c
  prefs.c
  debugger/dbg_window.c
  keypad/keypad_window.c
)

set(GNOME_APP_ID online.fujinet.go.intv.gnome)

add_executable(fujinet-go-intv-gnome ${GNOME_SOURCES})
# jzintv_core has one C++ piece (src/locutus/, per its own subMakefile --
# see core/CMakeLists.txt's enable_language(CXX)), so anything that links it
# needs libstdc++ on the link line even though every source file in THIS
# target is plain C. CMake normally infers the CXX linker (and libstdc++
# with it) automatically for a target with a C++ source among its direct
# dependencies -- core/tests/* all get this for free -- but this target's
# only sources are .c, and it only reaches jzintv_core transitively through
# intv_session's PUBLIC link, which is one hop too far for that inference
# in practice. Simplest fix: name the C++ runtime explicitly rather than
# fight CMake's language-inference heuristics.
#
# jzintv_core is ALSO named explicitly here (redundant with intv_session's
# own PUBLIC link to it) so it lands ahead of -lstdc++ on the final link
# line rather than after it: CMake appends a target's transitively-required
# libraries at the very end of the flattened link order, so without this,
# jzintv_core.a lands AFTER -lstdc++ -- fine for a lenient linker default,
# but a hard link failure ("DSO missing from command line") under a
# strict-by-default toolchain (a GNOME flatpak SDK build caught this one;
# --as-needed there means ld resolves left-to-right in one pass, so
# -lstdc++ appearing before the .o that actually needs std::locale's
# destructor is too late). Naming jzintv_core here moves it ahead of
# stdc++ in the explicit list, which is what fixes the order.
target_link_libraries(fujinet-go-intv-gnome PRIVATE intv_session jzintv_core
                      PkgConfig::ADW stdc++)
# debugger/dbg_window.c and keypad/keypad_window.c include their own headers
# by that path.
target_include_directories(fujinet-go-intv-gnome PRIVATE
                           "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${CMAKE_SOURCE_DIR}/core/debugger")
target_link_libraries(fujinet-go-intv-gnome PRIVATE m)
target_compile_options(fujinet-go-intv-gnome PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-intv-gnome PRIVATE
  INTV_APP_ID="${GNOME_APP_ID}"
  # Uninstalled runs fall back to the in-tree icon theme (intv_icon_name).
  INTV_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")

if(WITH_WEBVIEW)
  pkg_check_modules(WEBKIT IMPORTED_TARGET webkitgtk-6.0)
  if(WEBKIT_FOUND)
    target_link_libraries(fujinet-go-intv-gnome PRIVATE PkgConfig::WEBKIT)
    target_compile_definitions(fujinet-go-intv-gnome PRIVATE HAVE_WEBKIT)
  else()
    message(STATUS "GNOME frontend: webkitgtk-6.0 not found -- FujiNet config opens in the browser")
  endif()
endif()

install(TARGETS fujinet-go-intv-gnome RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/${GNOME_APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${GNOME_APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

# Catch a malformed launcher at build time rather than after installing.
find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_entry_gnome
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${GNOME_APP_ID}.desktop")
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_gnome
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${GNOME_APP_ID}.metainfo.xml")
endif()
