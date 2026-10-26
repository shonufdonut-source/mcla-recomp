# Merges every static library under SEARCH_DIRS into OUT with Apple's libtool.
file(GLOB_RECURSE archives "${SEARCH_DIRS}/*.a")
list(REMOVE_ITEM archives "${OUT}")
list(SORT archives)
list(LENGTH archives n)
message(STATUS "Merging ${n} archives into ${OUT}:")
foreach(a IN LISTS archives)
    message(STATUS "  ${a}")
endforeach()
file(REMOVE "${OUT}")
execute_process(
    COMMAND xcrun libtool -static -no_warning_for_no_symbols -o "${OUT}" ${archives}
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "libtool failed (${rc})")
endif()
