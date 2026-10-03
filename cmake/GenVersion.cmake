# Writes AppVersion.h from git state: 0.1.<commit count>+<short hash>[-dirty].
# Runs on every build (cheap), rewriting the header only when it changed, so the
# version bumps automatically with each commit and flags uncommitted builds.
execute_process(COMMAND git -C "${SRC}" rev-list --count HEAD
                OUTPUT_VARIABLE GIT_COUNT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${SRC}" rev-parse --short HEAD
                OUTPUT_VARIABLE GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${SRC}" status --porcelain
                OUTPUT_VARIABLE GIT_DIRTY ERROR_QUIET)

if("${GIT_COUNT}" STREQUAL "")
    set(GIT_COUNT 0)
    set(GIT_HASH "nogit")
endif()

set(SUFFIX "")
if(NOT "${GIT_DIRTY}" STREQUAL "")
    set(SUFFIX "-dirty")
endif()

set(VERSION_LINE "#pragma once\n#define ORCHESTRAL_DAW_VERSION \"0.1.${GIT_COUNT}+${GIT_HASH}${SUFFIX}\"\n")

file(WRITE "${OUT}.tmp" "${VERSION_LINE}")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")
