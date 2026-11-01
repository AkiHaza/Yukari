cmake_minimum_required(VERSION 3.22.1)
project(Yukari)

add_library(yukari SHARED
    main.cpp
    config.cpp
    service_match.cpp
    service_cache.cpp
    binder_hook.cpp
    feature_hook.cpp
    resource_hook.cpp
    logger.cpp
)

target_include_directories(yukari PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(yukari PRIVATE -ffunction-sections -fdata-sections -fvisibility=hidden)
target_link_options(yukari PRIVATE
    "-Wl,--gc-sections"
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map"
)
set_property(TARGET yukari APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/exports.map")

if (CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_STRIP)
    add_custom_command(TARGET yukari POST_BUILD
        COMMAND ${CMAKE_STRIP} --strip-unneeded $<TARGET_FILE:yukari>
        VERBATIM
    )
endif()

target_link_libraries(yukari
    log
    dl
)

set_target_properties(yukari PROPERTIES
    OUTPUT_NAME "yukari"
)
