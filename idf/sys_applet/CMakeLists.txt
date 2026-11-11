# ESP-IDF component: the ESP32-P4 system applet (get sys cpu / heap / stack /
# ...). Add <sys-applet-esp32p4>/idf/sys_applet to EXTRA_COMPONENT_DIRS.
#
# bfc comes from the parent project: declare it there with FetchContent
# (name `bfc`); this repo pins none, so a firmware keeps one bfc for all its
# libraries. If the project sets BFC_CONFIG_DIR (its bfc_config.hpp), it is on
# the include path too. bfc stays private: the public header does not use it.
set(SYS_APPLET_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(FetchContent)
    FetchContent_GetProperties(bfc)
    if(NOT bfc_POPULATED)
        FetchContent_Populate(bfc)
    endif()
endif()

idf_component_register(
    SRCS ${SYS_APPLET_ROOT}/src/sys_applet/applet.cpp
         ${SYS_APPLET_ROOT}/src/sys_applet/queries.cpp
    INCLUDE_DIRS ${SYS_APPLET_ROOT}/src
    PRIV_INCLUDE_DIRS compat
    REQUIRES freertos
    PRIV_REQUIRES lwip log esp_system esp_timer esp_app_format app_update heap
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_include_directories(${COMPONENT_LIB} PRIVATE ${bfc_SOURCE_DIR}/src)
    if(BFC_CONFIG_DIR)
        target_include_directories(${COMPONENT_LIB} PRIVATE ${BFC_CONFIG_DIR})
    endif()
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17 -fexceptions>)
