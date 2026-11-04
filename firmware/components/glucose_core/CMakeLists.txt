set(GLUCOSE_CORE_SOURCES
    glucose_core.c
    parse.c
    domain.c
    format.c
    backoff.c
)

if(COMMAND idf_component_register)
    idf_component_register(
        SRCS ${GLUCOSE_CORE_SOURCES}
        INCLUDE_DIRS include
    )
else()
    cmake_minimum_required(VERSION 3.24)
    project(glucose_core LANGUAGES C)

    option(GLUCOSE_CORE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
    option(GLUCOSE_CORE_ENABLE_COVERAGE "Enable GCC-compatible coverage instrumentation" OFF)

    add_library(glucose_core STATIC ${GLUCOSE_CORE_SOURCES})
    target_include_directories(glucose_core PUBLIC include)
    target_link_libraries(glucose_core PUBLIC m)

    add_executable(glucose_core_tests test/test_glucose_core.c)
    target_link_libraries(glucose_core_tests PRIVATE glucose_core)

    foreach(target glucose_core glucose_core_tests)
        set_target_properties(${target} PROPERTIES
            C_STANDARD 99
            C_STANDARD_REQUIRED YES
            C_EXTENSIONS NO
        )
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endforeach()

    if(GLUCOSE_CORE_ENABLE_SANITIZERS)
        if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "Sanitizers require GCC or Clang")
        endif()
        foreach(target glucose_core glucose_core_tests)
            target_compile_options(${target} PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
                -fno-sanitize-recover=all
            )
        endforeach()
        target_link_options(glucose_core_tests PRIVATE
            -fsanitize=address,undefined
            -fno-sanitize-recover=all
        )
    endif()

    if(GLUCOSE_CORE_ENABLE_COVERAGE)
        if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
            message(FATAL_ERROR "Coverage instrumentation and the gcov checker require GCC")
        endif()
        foreach(target glucose_core glucose_core_tests)
            target_compile_options(${target} PRIVATE --coverage -O0 -g)
        endforeach()
        target_link_options(glucose_core_tests PRIVATE --coverage)
    endif()

    enable_testing()
    add_test(NAME glucose_core COMMAND glucose_core_tests)
    set_tests_properties(glucose_core PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    )
endif()
