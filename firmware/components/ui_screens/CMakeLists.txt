set(UI_SCREENS_SOURCES
    ui_screens.c
)

if(COMMAND idf_component_register)
    idf_component_register(
        SRCS ${UI_SCREENS_SOURCES}
        INCLUDE_DIRS include
        REQUIRES glucose_core lvgl__lvgl runtime_core
    )
else()
    cmake_minimum_required(VERSION 3.24)
    project(ui_screens LANGUAGES C)

    if(NOT TARGET lvgl)
        message(FATAL_ERROR "Host builds must provide an LVGL target before adding ui_screens")
    endif()

    add_library(ui_screens STATIC ${UI_SCREENS_SOURCES})
    target_include_directories(ui_screens
        PUBLIC include ../glucose_core/include ../runtime_core/include
    )
    target_link_libraries(ui_screens PUBLIC lvgl)
    set_target_properties(ui_screens PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    target_compile_options(ui_screens PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
