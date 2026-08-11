# macOS frontend: native AppKit (Objective-C) over the shared session core.
# Built only on Apple hosts. NOT built or run-verified in the environment
# this was written in (Linux) -- see TODO's own note on this.

if(NOT APPLE)
  return()
endif()

enable_language(OBJC)

# Same rendered art as the other frontends (tools/icons/make-icons.py), just
# packed as .icns instead of hicolor PNGs. Listing it as a source with a
# MACOSX_PACKAGE_LOCATION is what makes CMake copy it into Contents/Resources
# for both the Xcode and Ninja/Makefiles generators.
set(MACOS_ICON "${CMAKE_SOURCE_DIR}/data/icons/fujinet-go-intv.icns")
set_source_files_properties(${MACOS_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

add_executable(fujinet-go-intv-macos MACOSX_BUNDLE
  main.m
  AppDelegate.m
  DisplayView.m
  debugger/DebuggerWindow.m
  keypad/KeypadWindow.m
  ${MACOS_ICON}
)
set_target_properties(fujinet-go-intv-macos PROPERTIES
  OUTPUT_NAME "FujiNet Go Intv"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_ICON_FILE "fujinet-go-intv.icns"
)
# jzintv_core has one C++ piece (src/locutus/, per its own subMakefile --
# see core/CMakeLists.txt's enable_language(CXX)), so anything that links it
# needs libc++ on the link line even though every source file in THIS
# target is Objective-C (.m). CMake normally infers the CXX/ObjC++ linker
# (and libc++ with it) automatically for a target with a C++ source among
# its direct dependencies, but this target's sources are all .m and it only
# reaches jzintv_core transitively through intv_session's PUBLIC link --
# one hop too far for that inference in practice. Same fix as the GNOME/
# Windows frontends' own CMakeLists.txt (see their file comments): name
# jzintv_core explicitly (ahead of c++ in the list, so it does not land
# after it on the final link line -- the GNOME flatpak build caught that
# exact ordering trap under a stricter --as-needed toolchain) and the C++
# runtime explicitly, rather than fight CMake's language-inference
# heuristics. Apple's C++ runtime is libc++, linked as -lc++, not -lstdc++.
target_link_libraries(fujinet-go-intv-macos PRIVATE
  intv_session
  jzintv_core
  c++
  "-framework Cocoa"
  "-framework CoreVideo"
  "-framework QuartzCore"
  "-framework UniformTypeIdentifiers"
  "-framework WebKit"
)
target_include_directories(fujinet-go-intv-macos PRIVATE
                           "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${CMAKE_SOURCE_DIR}/core/debugger")
target_compile_options(fujinet-go-intv-macos PRIVATE -Wall -Wextra -fobjc-arc)

# Pack the FujiNet runtime into the bundle: intvsession's own paths.c
# (paths_provision_fujinet) searches exe_dir() -- which for a bundle is
# Contents/MacOS -- for libfujinet.dylib and exe_dir()/fujinet/fnconfig.ini,
# so unlike the CoCo/MSX ports (whose cocosession_paths/msxsession_paths
# take an explicit override) this lands the runtime right next to the
# executable rather than in Contents/Frameworks, needing no session API
# change. The runtime is built by this same build
# (cmake/FujiNetRuntime.cmake), so wait for it; a tree left by an earlier
# manual run of the script is used as-is.
if(TARGET fujinet-runtime)
  add_dependencies(fujinet-go-intv-macos fujinet-runtime)
endif()
if(TARGET fujinet-runtime OR EXISTS "${FUJINET_OUT}/libfujinet.dylib")
  add_custom_command(TARGET fujinet-go-intv-macos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "${FUJINET_OUT}/libfujinet.dylib"
            "$<TARGET_FILE_DIR:fujinet-go-intv-macos>/"
    COMMAND ${CMAKE_COMMAND} -E copy "${FUJINET_OUT}/fnconfig.ini"
            "$<TARGET_FILE_DIR:fujinet-go-intv-macos>/fujinet/fnconfig.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${FUJINET_OUT}/data"
            "$<TARGET_FILE_DIR:fujinet-go-intv-macos>/fujinet/data"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${FUJINET_OUT}/SD"
            "$<TARGET_FILE_DIR:fujinet-go-intv-macos>/fujinet/SD"
    COMMENT "Bundling the FujiNet runtime into the app"
  )
else()
  message(STATUS "macOS frontend: FujiNet runtime disabled; the app will run without FujiNet")
endif()

install(TARGETS fujinet-go-intv-macos BUNDLE DESTINATION .)
