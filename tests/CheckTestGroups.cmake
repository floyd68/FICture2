# Fails when FICture2Tests contains tests that no CTest entry runs: every
# test name must start with one of the prefixes registered by
# fic2_add_test_group (tests/CMakeLists.txt).
#
# Usage: cmake -DTESTS_EXE=<FICture2Tests> -DPREFIXES=<p1|p2|...> -P CheckTestGroups.cmake

if(NOT TESTS_EXE OR NOT PREFIXES)
    message(FATAL_ERROR "CheckTestGroups.cmake needs TESTS_EXE and PREFIXES")
endif()
string(REPLACE "|" ";" prefixes "${PREFIXES}")

execute_process(
    COMMAND "${TESTS_EXE}" --list
    OUTPUT_VARIABLE listing
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${TESTS_EXE} --list failed (${result})")
endif()

string(REPLACE "\r" "" listing "${listing}")
string(REPLACE "\n" ";" names "${listing}")
set(count 0)
set(unregistered "")
foreach(name IN LISTS names)
    string(STRIP "${name}" name)
    if(name STREQUAL "")
        continue()
    endif()
    math(EXPR count "${count} + 1")
    set(found FALSE)
    foreach(prefix IN LISTS prefixes)
        string(FIND "${name}" "${prefix}" position)
        if(position EQUAL 0)
            set(found TRUE)
            break()
        endif()
    endforeach()
    if(NOT found)
        list(APPEND unregistered "${name}")
    endif()
endforeach()

if(count EQUAL 0)
    message(FATAL_ERROR "${TESTS_EXE} --list printed no tests")
endif()
if(unregistered)
    list(JOIN unregistered "\n  " text)
    message(FATAL_ERROR
        "These tests are compiled but run by no CTest entry; register their prefix "
        "with fic2_add_test_group in tests/CMakeLists.txt:\n  ${text}")
endif()
message(STATUS "All ${count} FICture2Tests tests are run by a registered CTest group.")
