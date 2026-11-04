cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Include only main and its dependency graph; this keeps the release deterministic and lean.
idf_build_set_property(MINIMAL_BUILD ON)
project(libre_desk_mvp)
