# The FujiNet runtime (libfujinet), built from the pinned fujinet-firmware
# checkout and bundled with the app.
#
# STATUS: skeleton only (M3 in the implementation plan). This is what will
# make the desktop app self-contained: the emulator connects out to a real
# FujiNet over BoIP (Bus Over IP) on loopback -- see src/fujinet/fn_sock.c in
# the staged jzIntv tree, and cocosession's own fujinet_runtime.c for the
# dlopen/thread pattern this will follow. Until M3 lands, configure with the
# default (below) to skip it -- the core and no_sdl_link_test do not need it.
#
# Modelled on fujinet-go-coco-desktop/cmake/FujiNetRuntime.cmake: the sources
# are provided like any other dependency (cmake/Dependencies.cmake,
# FUJINET_COMMIT/FUJINET_URL), and a tools/fujinet/build-fujinet-desktop.sh
# will stage, patch (FUJINET_TARGET=RS232 -- see the plan) and build them into
# tools/fujinet/work/out.

option(WITH_FUJINET "Build and bundle the FujiNet runtime" OFF)

if(WITH_FUJINET)
  message(FATAL_ERROR
    "WITH_FUJINET=ON is not implemented yet (see cmake/FujiNetRuntime.cmake "
    "and the M3 milestone in the project plan). Configure with "
    "-DWITH_FUJINET=OFF for now.")
endif()

message(STATUS "FujiNet runtime disabled (WITH_FUJINET=OFF); "
               "the emulator will run without FujiNet")
