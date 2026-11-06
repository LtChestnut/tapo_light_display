cmake_minimum_required(VERSION 3.16.0)
# Pin the version so IDF doesn't try to `git describe` (fails while the repo has no commits).
set(PROJECT_VER "1.0.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(secondary_sleep)
