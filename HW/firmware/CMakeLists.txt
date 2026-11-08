cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sentiframe C)
target_include_directories(app PRIVATE include third_party/bmi270)
target_sources(app PRIVATE
    src/main.c src/platform.c src/transport.c
    src/core/protocol.c src/core/fall.c src/core/rx.c
    src/drivers/charger.c src/drivers/ppg.c src/drivers/imu.c
    third_party/bmi270/bmi2.c third_party/bmi270/bmi270.c)
target_compile_options(app PRIVATE -Wall -Wextra -Werror)
