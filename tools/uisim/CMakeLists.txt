cmake_minimum_required(VERSION 3.24)
project(libre_desk_uisim LANGUAGES C CXX ASM)

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EXPECTED_LVGL_SOURCE_DIR "${REPOSITORY_ROOT}/firmware/managed_components/lvgl__lvgl")
set(LVGL_SOURCE_DIR "${EXPECTED_LVGL_SOURCE_DIR}"
    CACHE PATH "Path to the exact LVGL 9.5.0 source tree")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/verify_lvgl_pin.py"
            --source "${LVGL_SOURCE_DIR}"
            --expected-source "${EXPECTED_LVGL_SOURCE_DIR}"
            --lock "${REPOSITORY_ROOT}/firmware/dependencies.lock"
    RESULT_VARIABLE LVGL_PIN_RESULT
    OUTPUT_VARIABLE LVGL_PIN_OUTPUT
    ERROR_VARIABLE LVGL_PIN_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT LVGL_PIN_RESULT EQUAL 0)
    message(FATAL_ERROR "${LVGL_PIN_ERROR}")
endif()
message(STATUS "${LVGL_PIN_OUTPUT}")

set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE FILEPATH "" FORCE)
set(LV_BUILD_USE_KCONFIG OFF CACHE BOOL "" FORCE)
set(LV_BUILD_SET_CONFIG_OPTS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory("${LVGL_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/lvgl")
add_custom_target(verify_managed_lvgl_pin ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/verify_lvgl_pin.py"
            --source "${LVGL_SOURCE_DIR}"
            --expected-source "${EXPECTED_LVGL_SOURCE_DIR}"
            --lock "${REPOSITORY_ROOT}/firmware/dependencies.lock"
    COMMENT "Verifying canonical managed LVGL source-tree digest"
    VERBATIM
)
add_dependencies(lvgl verify_managed_lvgl_pin)
add_subdirectory(
    "${REPOSITORY_ROOT}/firmware/components/ui_screens"
    "${CMAKE_CURRENT_BINARY_DIR}/ui_screens"
)

add_executable(uisim main.c)
target_link_libraries(uisim PRIVATE ui_screens lvgl m)
target_compile_options(uisim PRIVATE -Wall -Wextra -Werror -pedantic)
set_target_properties(uisim PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

enable_testing()
add_executable(ui_screens_tests test_ui_screens.c)
target_link_libraries(ui_screens_tests PRIVATE ui_screens lvgl m)
target_compile_options(ui_screens_tests PRIVATE -Wall -Wextra -Werror -pedantic)
set_target_properties(ui_screens_tests PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)
add_test(NAME ui_screens_model COMMAND ui_screens_tests)
add_test(
    NAME lvgl_pin_verifier
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_verify_lvgl_pin.py"
)
add_test(
    NAME output_semantic_helpers
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_output_semantics.py"
)
add_test(
    NAME render_lock_and_publication
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_render_lock.py"
)
