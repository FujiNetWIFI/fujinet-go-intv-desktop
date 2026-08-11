# Cross-compile the Windows frontend from Linux with mingw-w64.
#
# The maintainer has no Windows machine, so this is how Windows changes to
# the session/frontend code get compiled and linked before CI (which builds
# natively under MSYS2/UCRT64) ever sees them. Wine can then run the result
# for a smoke test. Modelled directly on the sibling MSX/CoCo ports' own
# copy of this file; unlike those, jzintv_core is pure C with no SDL
# dependency of its own, so the only cross-built third-party piece this
# target actually needs is SDL3 itself (for intv_session's gamepad/audio
# backends) -- get it from SDL's own prebuilt mingw devel archive rather
# than building it from source:
#
#   curl -LO https://github.com/libsdl-org/SDL/releases/download/release-3.4.14/SDL3-devel-3.4.14-mingw.tar.gz
#   tar xzf SDL3-devel-3.4.14-mingw.tar.gz
#
#   cmake -B build-win -G Ninja -DFRONTEND=windows \
#       -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchains/mingw-w64.cmake \
#       -DCMAKE_PREFIX_PATH="$PWD/../SDL3-3.4.14/x86_64-w64-mingw32" \
#       -DWITH_FUJINET=OFF -DWITH_INTV_ROMS=OFF
#   cmake --build build-win
#
# (-DWITH_FUJINET=OFF for the smoke test: FujiNet's own dependency chain
#  -- expat/zlib/mbedTLS -- is not cross-built here; see the CoCo/MSX
#  ports' own copy of this file for that recipe if a full cross-build with
#  FujiNet included is ever needed.)
#
# Running it under wine wants SDL3.dll beside the exe:
#
#   cp ../SDL3-3.4.14/x86_64-w64-mingw32/bin/SDL3.dll build-win/frontends/windows/
#   wine build-win/frontends/windows/fujinet-go-intv-windows.exe

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

# Search only the cross sysroot and whatever prefixes the caller named, so
# host headers and libraries cannot leak into a Windows build.
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX} ${CMAKE_PREFIX_PATH})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
