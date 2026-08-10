# Provide the jzIntv sources (see cmake/Dependencies.cmake) and stage the
# subset we build into core/jzintv-generated, which is what core/CMakeLists.txt
# compiles.
#
# jzIntv builds with hand-written Makefiles, one per platform, selected by
# which *files* implement gfx/snd/event/plat -- gfx_sdl2.c vs gfx_null.c,
# never #ifdef'd inside a shared file. That means the desktop core needs no
# patching to drop SDL: core/jzintv/desktop/{gfx,snd,event,plat}_desktop.c
# simply take the place of jzIntv's own gfx_sdl2.c/snd_sdl.c/event_sdl2.c/
# plat_sdl.c/main_sdl.c in the source list, and none of the SDL-only headers
# (sdl_jzintv.h) are ever reached from a file we compile.
#
# What the staged tree DOES need, on top of the FujiNet patch:
#   - Normalizing to LF line endings. jzIntv's own tree mixes CRLF (from its
#     Windows-first history) with a handful of stray LF lines -- literally
#     enough to make `patch` refuse hunks with "different line endings" even
#     though the content applies cleanly. tools/jzintv/jzintv-fujinet.patch is
#     generated against an LF-normalized tree for exactly this reason; see the
#     patch file's own header.
#
# The copy is done in CMake rather than by a shell script for the same reason
# cmake/FujiNetRuntime.cmake gives for its own staging: a native Windows
# python (used by tools/jzintv/patch-staged-tree.py) is directly invocable
# where a .sh is not.
#
# Staging is automatic: it runs when the staged tree is missing, when the
# source has moved, when the patch script or the FujiNet patch changes, or on
# demand with -DJZINTV_RESTAGE=ON (which is also how to pick up uncommitted
# edits in a working checkout pointed at by JZINTV_SRC).

set(JZINTV_GEN "${CMAKE_SOURCE_DIR}/core/jzintv-generated")
set(JZINTV_PATCH_SCRIPT "${CMAKE_SOURCE_DIR}/tools/jzintv/patch-staged-tree.py")
set(JZINTV_FUJINET_PATCH "${CMAKE_SOURCE_DIR}/tools/jzintv/jzintv-fujinet.patch")

option(JZINTV_RESTAGE "Re-stage jzIntv sources from the source checkout" OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

intv_provide_dependency(
  NAME jzIntv
  PATH third_party/jzintv
  ARCHIVE "${JZINTV_URL}"
  SHA256 "${JZINTV_SHA256}"
  STRIP "jzintv-${JZINTV_VERSION}-src"
  SENTINEL src/jzintv.c
  OVERRIDE JZINTV_SRC
  RESULT JZINTV_DIR)

# What the staged tree was made from: the source identity plus hashes of the
# things that transform it, so that editing a transform re-stages rather than
# silently leaving the old result in place.
set(_jzintv_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${JZINTV_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _jzintv_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT _jzintv_head)
  # The normal case: a release zip has no git metadata. Identify it by the
  # pinned version and the path it was unpacked to.
  set(_jzintv_head "${JZINTV_VERSION} ${JZINTV_DIR}")
endif()
file(SHA256 "${JZINTV_PATCH_SCRIPT}" _jzintv_patch_script_hash)
file(SHA256 "${JZINTV_FUJINET_PATCH}" _jzintv_fujinet_patch_hash)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _jzintv_stage_hash)
set(_jzintv_want
    "${_jzintv_head} ${_jzintv_patch_script_hash} ${_jzintv_fujinet_patch_hash} ${_jzintv_stage_hash}")

set(_jzintv_staged "")
if(EXISTS "${JZINTV_GEN}/.source-info")
  file(READ "${JZINTV_GEN}/.source-info" _jzintv_staged)
  string(STRIP "${_jzintv_staged}" _jzintv_staged)
endif()

set(_jzintv_sentinel "${JZINTV_GEN}/src/jzintv.c")

if(JZINTV_RESTAGE OR NOT EXISTS "${_jzintv_sentinel}"
   OR NOT _jzintv_staged STREQUAL _jzintv_want)
  message(STATUS "Staging jzIntv sources from ${JZINTV_DIR}")
  file(REMOVE_RECURSE "${JZINTV_GEN}")
  file(MAKE_DIRECTORY "${JZINTV_GEN}")

  if(NOT IS_DIRECTORY "${JZINTV_DIR}/src")
    message(FATAL_ERROR "jzIntv source ${JZINTV_DIR} is missing src/")
  endif()

  file(COPY "${JZINTV_DIR}/src"
       DESTINATION "${JZINTV_GEN}"
       PATTERN ".git" EXCLUDE
       PATTERN "*.o" EXCLUDE
       REGEX "\\.(o|a|so|d)$" EXCLUDE)

  if(NOT EXISTS "${_jzintv_sentinel}")
    message(FATAL_ERROR "jzIntv staging failed (source: ${JZINTV_DIR})")
  endif()

  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${JZINTV_PATCH_SCRIPT}" "${JZINTV_GEN}"
            "${JZINTV_FUJINET_PATCH}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    # Leave nothing half-patched behind: the next configure must re-stage from
    # pristine sources rather than trying to patch a partial tree.
    file(REMOVE_RECURSE "${JZINTV_GEN}")
    message(FATAL_ERROR
      "jzIntv staging patch failed. The pinned version (${JZINTV_VERSION}) "
      "and the anchors in\n  ${JZINTV_FUJINET_PATCH}\nhave to agree; a source "
      "checkout of a different version is the usual cause.\n${_out}${_err}")
  endif()

  file(WRITE "${JZINTV_GEN}/.source-info" "${_jzintv_want}\n")
endif()

message(STATUS "jzIntv ${JZINTV_VERSION} staged from ${JZINTV_DIR}")
