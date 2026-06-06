cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf91x_tracker)

target_sources(app PRIVATE
    src/main.c
    src/ble_scanner.c
    src/cell_scanner.c
    src/gnss_module.c
    src/wifi_scanner.c
    src/http_client.c
    src/sensor_module.c
)

target_include_directories(app PRIVATE src)
