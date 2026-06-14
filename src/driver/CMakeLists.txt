set(MODULE libvirtualdisplay_driver)
set(MODULE_ALIAS libvirtualdisplay::driver)

file(GLOB HEADER_LIST CONFIGURE_DEPENDS "include/virtual_display/driver/*.h")
file(GLOB SOURCE_LIST CONFIGURE_DEPENDS "*.cpp")

add_library(${MODULE} STATIC ${HEADER_LIST} ${SOURCE_LIST})
add_library(${MODULE_ALIAS} ALIAS ${MODULE})

target_include_directories(${MODULE} PUBLIC include)
target_compile_features(${MODULE} PUBLIC cxx_std_20)

if(WIN32)
    target_link_libraries(${MODULE} PRIVATE setupapi)
endif()

# Host tool that renders the committed driver INF from its template by substituting the
# security-critical values (control interface GUID, SDDL) from the device_identity single
# source of truth. It is pure C++ (no Win32), so it builds and runs in the normal/test
# build. `regen_driver_inf` rewrites the committed INF; the test suite runs it in --check
# mode (see tests/CMakeLists.txt) so the shipped INF can never drift from the C++ constants.
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(SUNSHINE_DRIVER_INF_TEMPLATE
            "${CMAKE_CURRENT_SOURCE_DIR}/windows_driver/SunshineVirtualDisplayDriver.inf.in")
    set(SUNSHINE_DRIVER_INF_COMMITTED
            "${CMAKE_CURRENT_SOURCE_DIR}/windows_driver/SunshineVirtualDisplayDriver.inf")

    add_executable(gen_driver_inf "${PROJECT_SOURCE_DIR}/tools/gen_driver_inf.cpp")
    target_link_libraries(gen_driver_inf PRIVATE libvirtualdisplay::driver)
    target_compile_features(gen_driver_inf PRIVATE cxx_std_20)

    add_custom_target(regen_driver_inf
            COMMAND gen_driver_inf
                "${SUNSHINE_DRIVER_INF_TEMPLATE}"
                "${SUNSHINE_DRIVER_INF_COMMITTED}"
            COMMENT "Regenerating SunshineVirtualDisplayDriver.inf from device_identity")
endif()

option(BUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER "Build the Windows UMDF/IddCx Sunshine virtual display driver" OFF)
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(_libvirtualdisplay_tool_default ON)
else()
    set(_libvirtualdisplay_tool_default OFF)
endif()
option(BUILD_VIRTUALDISPLAY_TOOLS "Build virtualdisplay CLI and broker executables" ${_libvirtualdisplay_tool_default})
option(BUILD_VIRTUALDISPLAY_PROBE "Build virtual display driver probe executable" ${_libvirtualdisplay_tool_default})
option(BUILD_VIRTUALDISPLAY_VULKAN_LAYER "Build the Vulkan implicit layer that exposes HDR surface formats on virtual displays" ${_libvirtualdisplay_tool_default})
unset(_libvirtualdisplay_tool_default)

if(BUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER)
    add_subdirectory(windows_driver)
endif()

set(_libvirtualdisplay_tool_targets)

if(WIN32 AND BUILD_VIRTUALDISPLAY_PROBE)
    add_executable(virtualdisplay_probe "${PROJECT_SOURCE_DIR}/tools/virtualdisplay_probe.cpp")
    target_link_libraries(virtualdisplay_probe PRIVATE libvirtualdisplay::driver advapi32 d3d11 dxgi mscms)
    target_compile_features(virtualdisplay_probe PRIVATE cxx_std_20)
    install(TARGETS virtualdisplay_probe RUNTIME DESTINATION tools)
    list(APPEND _libvirtualdisplay_tool_targets virtualdisplay_probe)
endif()

if(WIN32 AND BUILD_VIRTUALDISPLAY_TOOLS)
    add_executable(virtualdisplay "${PROJECT_SOURCE_DIR}/tools/virtualdisplay.cpp")
    target_link_libraries(virtualdisplay PRIVATE libvirtualdisplay::driver advapi32 shell32 newdev)
    target_compile_features(virtualdisplay PRIVATE cxx_std_20)
    install(TARGETS virtualdisplay RUNTIME DESTINATION tools)
    list(APPEND _libvirtualdisplay_tool_targets virtualdisplay)

    add_executable(virtualdisplay_broker "${PROJECT_SOURCE_DIR}/tools/virtualdisplay_broker.cpp")
    target_link_libraries(virtualdisplay_broker PRIVATE libvirtualdisplay::driver advapi32 userenv wtsapi32)
    target_compile_features(virtualdisplay_broker PRIVATE cxx_std_20)
    install(TARGETS virtualdisplay_broker RUNTIME DESTINATION tools)
    list(APPEND _libvirtualdisplay_tool_targets virtualdisplay_broker)
endif()

if(WIN32 AND BUILD_VIRTUALDISPLAY_VULKAN_LAYER)
    add_library(vk_layer_sunshine_hdr SHARED vulkan_layer/vk_layer_sunshine_hdr.cpp)
    set_target_properties(vk_layer_sunshine_hdr PROPERTIES OUTPUT_NAME "VkLayer_sunshine_hdr" PREFIX "")
    target_link_libraries(vk_layer_sunshine_hdr PRIVATE user32)
    target_compile_features(vk_layer_sunshine_hdr PRIVATE cxx_std_20)
    add_custom_command(TARGET vk_layer_sunshine_hdr POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/vulkan_layer/VkLayer_sunshine_hdr.json"
            "$<TARGET_FILE_DIR:vk_layer_sunshine_hdr>/VkLayer_sunshine_hdr.json")
    install(TARGETS vk_layer_sunshine_hdr RUNTIME DESTINATION vulkan-layer)
    install(FILES vulkan_layer/VkLayer_sunshine_hdr.json DESTINATION vulkan-layer)
    list(APPEND _libvirtualdisplay_tool_targets vk_layer_sunshine_hdr)
endif()

if(_libvirtualdisplay_tool_targets)
    add_custom_target(virtualdisplay_tools DEPENDS ${_libvirtualdisplay_tool_targets})
endif()
unset(_libvirtualdisplay_tool_targets)
