# KDE frontend: plain Qt6 Widgets -- deliberately NOT KDE Frameworks. Breeze
# arrives through the platform theme, and staying framework-free keeps this
# frontend reusable. Found-or-skipped so a machine without Qt can still
# build the core and the GNOME side.

# core/CMakeLists.txt already calls enable_language(CXX) (for
# src/locutus/) and sets CMAKE_CXX_STANDARD 14 -- but as a plain variable
# inside that subdirectory's own scope, which does not propagate to this
# sibling directory. Qt6 Widgets needs C++17 at minimum, so both are set
# fresh here.
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.4 COMPONENTS Widgets QUIET)

if(NOT Qt6_FOUND)
  message(STATUS "KDE frontend: Qt6 Widgets not found -- skipping")
  return()
endif()

set(CMAKE_AUTOMOC ON)

set(KDE_APP_ID online.fujinet.go.intv.kde)

add_executable(fujinet-go-intv-kde
  main.cpp
  MainWindow.cpp
  DisplayWidget.cpp
  FujiNetWindows.cpp
  SettingsDialog.cpp
  debugger/DebuggerWindow.cpp
  keypad/KeypadWindow.cpp
)

# See frontends/gnome/CMakeLists.txt's own comment on why this is needed:
# jzintv_core has one C++ translation unit (src/locutus/), and this target
# only reaches it transitively through intv_session's PUBLIC link -- one
# hop too far for CMake's linker-language inference, which would otherwise
# pick the C++ linker automatically the way core/tests/* (linking
# jzintv_core directly) gets for free. Here it is moot either way (this
# target's own sources are already C++, so CMake already picks the C++
# linker on its own) but named explicitly for the same reason documented
# there: don't rely on inference nobody has to reason about twice.
target_link_libraries(fujinet-go-intv-kde PRIVATE intv_session Qt6::Widgets)
target_include_directories(fujinet-go-intv-kde PRIVATE
                           "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${CMAKE_SOURCE_DIR}/core/debugger")
target_compile_options(fujinet-go-intv-kde PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-intv-kde PRIVATE
  INTV_ICON_FILE="${CMAKE_SOURCE_DIR}/data/icons/hicolor/256x256/apps/fujinet-go-intv.png")

if(WITH_WEBVIEW)
  # QtWebEngine is enormous and absent on many systems, so it is never a
  # hard dependency: without it the FujiNet config opens in the system
  # browser.
  find_package(Qt6 6.4 COMPONENTS WebEngineWidgets QUIET)
  if(Qt6WebEngineWidgets_FOUND)
    target_link_libraries(fujinet-go-intv-kde PRIVATE Qt6::WebEngineWidgets)
    target_compile_definitions(fujinet-go-intv-kde PRIVATE HAVE_WEBENGINE)
  else()
    message(STATUS "KDE frontend: QtWebEngine not found -- FujiNet config opens in the browser")
  endif()
endif()

install(TARGETS fujinet-go-intv-kde RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/${KDE_APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${KDE_APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_entry_kde
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${KDE_APP_ID}.desktop")
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_kde
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${KDE_APP_ID}.metainfo.xml")
endif()
