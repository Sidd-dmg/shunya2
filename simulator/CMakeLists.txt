cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()
set(CMAKE_POLICY_DEFAULT_CMP0177 NEW)
project(shunya_hmi_simulator VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Deliberately fetch exact upstream revisions; the simulator never reads .pio.
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v8.4.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(lvgl)
# Upstream exposes examples/demos as normal build targets. They are not part of
# this simulator and can conflict with deliberately disabled widgets.
set_target_properties(lvgl_examples lvgl_demos PROPERTIES EXCLUDE_FROM_ALL TRUE)

# lv_drivers v8.3 is the last LVGL 8 SDL driver release and is compatible with
# LVGL 8.4. Fetch only: its upstream CMake file builds every hardware driver.
FetchContent_Declare(
    lv_drivers
    GIT_REPOSITORY https://github.com/lvgl/lv_drivers.git
    GIT_TAG v8.3.0
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(lv_drivers)
if(NOT lv_drivers_POPULATED)
    FetchContent_Populate(lv_drivers)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
find_library(SDL2_LIBRARY NAMES SDL2 PATHS ${SDL2_LIBRARY_DIRS}
             NO_DEFAULT_PATH REQUIRED)

add_library(hmi_sdl_driver STATIC
    "${lv_drivers_SOURCE_DIR}/sdl/sdl.c"
    "${lv_drivers_SOURCE_DIR}/sdl/sdl_common.c"
)
target_include_directories(hmi_sdl_driver
    PUBLIC "${lv_drivers_SOURCE_DIR}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" ${SDL2_INCLUDE_DIRS}
)
target_compile_definitions(hmi_sdl_driver PRIVATE
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE
)
target_link_libraries(hmi_sdl_driver PUBLIC lvgl "${SDL2_LIBRARY}")

add_executable(shunya-hmi-simulator
    src/main.cpp
    src/mock_gateway.cpp
    src/ui_backend_sim.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/display/ui/generated/ui.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/display/ui/generated/ui_img_shunya_logo.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/display/ui/ui_controller.cpp"
)
if(WIN32)
    enable_language(RC)
    target_sources(shunya-hmi-simulator PRIVATE windows.rc)
endif()
target_include_directories(shunya-hmi-simulator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/display/ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/display/ui/generated"
)
target_compile_definitions(shunya-hmi-simulator PRIVATE
    SDL_MAIN_HANDLED
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE
    _WIN32_WINNT=0x0A00
)
target_link_libraries(shunya-hmi-simulator PRIVATE
    hmi_sdl_driver
    lvgl
    "${SDL2_LIBRARY}"
)

