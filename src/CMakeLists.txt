if(IDF_TARGET STREQUAL "esp32s3")
    file(GLOB_RECURSE app_sources
        "${CMAKE_SOURCE_DIR}/src/gateway/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/common/*.c"
    )
    idf_component_register(
        SRCS ${app_sources}
        INCLUDE_DIRS "gateway" "common"
    )
elseif(SHUNYA_ZB_ROLE STREQUAL "sensor")
    idf_component_register(
        SRCS
            "sensor/main.c"
            "sensor/sht40.c"
            "sensor/zb_sensor.c"
        INCLUDE_DIRS
            "sensor"
            "common"
        REQUIRES
            nvs_flash
            driver
            esp_rom
            esp_timer
            esp_event
            esp_adc
    )
else()
    idf_component_register(
        SRCS
            "hub/main.c"
            "hub/hub_uart.c"
            "hub/slots.c"
            "hub/zigbee_coord.c"
            "common/link_protocol.c"
        INCLUDE_DIRS
            "hub"
            "common"
        REQUIRES
            nvs_flash
            driver
            esp_timer
            esp_event
    )
endif()
