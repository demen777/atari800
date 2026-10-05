# Run on every build (see the build_stamp target in CMakeLists.txt): writes
# build_stamp.h with the local date and time and the git commit being built,
# marked "+" when the working tree has uncommitted changes.
#   cmake -DOUT=<header> -DSRC=<source dir> -P build_stamp.cmake

string(TIMESTAMP stamp "%Y-%m-%d %H:%M")

execute_process(COMMAND git rev-parse --short=8 HEAD
        WORKING_DIRECTORY ${SRC}
        OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE rc)
if (rc EQUAL 0)
    execute_process(COMMAND git status --porcelain --untracked-files=no
            WORKING_DIRECTORY ${SRC}
            OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (dirty)
        set(commit "${commit}+")
    endif ()
    set(stamp "${stamp} ${commit}")
endif ()

file(WRITE ${OUT} "#define BUILD_STAMP \"${stamp}\"\n")
