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

option(BUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER "Build the Windows UMDF/IddCx Sunshine virtual display driver" OFF)
option(BUILD_SUNSHINE_REMOTE_DISPLAY_DRIVER "Build the per-terminal-session UMDF/IddCx Sunshine remote display driver" OFF)
option(SUNSHINE_REMOTE_DISPLAY_PROOF_INITIAL_HDR "Compile the proof-only initial HDR path into the remote display driver" OFF)
option(SUNSHINE_REMOTE_DISPLAY_PROOF_DUO_HDR_WIRE "Compile the proof-only Duo-equivalent HDR wire contract into the remote display driver" OFF)
option(SUNSHINE_REMOTE_DISPLAY_PROOF_WCG_PRIME_HDR "Compile the proof-only SDR-WCG source-mode priming path into the remote display driver" OFF)
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(_libvirtualdisplay_tool_default ON)
else()
    set(_libvirtualdisplay_tool_default OFF)
endif()
option(BUILD_VIRTUALDISPLAY_TOOLS "Build virtualdisplay CLI and broker executables" ${_libvirtualdisplay_tool_default})
option(BUILD_VIRTUALDISPLAY_PROBE "Build virtual display driver probe executable" ${_libvirtualdisplay_tool_default})
option(BUILD_VIRTUALDISPLAY_VULKAN_LAYER "Build the Vulkan implicit layer that exposes HDR surface formats on virtual displays" ${_libvirtualdisplay_tool_default})
unset(_libvirtualdisplay_tool_default)

# Host tool that renders the committed driver INF from its template by substituting the
# security-critical values (control interface GUID, SDDL) from the device_identity single
# source of truth. It is pure C++ (no Win32), so it builds and runs in the normal/test
# build. `regen_driver_inf` rewrites the committed INF; the test suite runs it in --check
# mode (see tests/CMakeLists.txt) so the shipped INF can never drift from the C++ constants.
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME OR BUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER OR BUILD_SUNSHINE_REMOTE_DISPLAY_DRIVER)
    set(SUNSHINE_DRIVER_INF_TEMPLATE
            "${CMAKE_CURRENT_SOURCE_DIR}/windows_driver/SunshineVirtualDisplayDriver.inf.in")
    set(SUNSHINE_DRIVER_INF_COMMITTED
            "${CMAKE_CURRENT_SOURCE_DIR}/windows_driver/SunshineVirtualDisplayDriver.inf")
    set(SUNSHINE_REMOTE_DRIVER_INF_TEMPLATE
            "${CMAKE_CURRENT_SOURCE_DIR}/windows_driver/SunshineRemoteDisplayDriver.inf.in")
    set(SUNSHINE_REMOTE_DRIVER_INF_COMMITTED
            "${CMAKE_CURRENT_SOURCE_DIR}/windows_driver/SunshineRemoteDisplayDriver.inf")

    add_executable(gen_driver_inf "${PROJECT_SOURCE_DIR}/tools/gen_driver_inf.cpp")
    target_link_libraries(gen_driver_inf PRIVATE libvirtualdisplay::driver)
    target_compile_features(gen_driver_inf PRIVATE cxx_std_20)

    add_custom_target(regen_driver_inf
            COMMAND gen_driver_inf
                "${SUNSHINE_DRIVER_INF_TEMPLATE}"
                "${SUNSHINE_DRIVER_INF_COMMITTED}"
            COMMAND gen_driver_inf
                "${SUNSHINE_REMOTE_DRIVER_INF_TEMPLATE}"
                "${SUNSHINE_REMOTE_DRIVER_INF_COMMITTED}"
            COMMENT "Regenerating SunshineVirtualDisplayDriver.inf from device_identity")

    set(_libvirtualdisplay_driver_version_source "${LIBVIRTUALDISPLAY_PACKAGE_VERSION}")
    string(REGEX REPLACE "^v" "" _libvirtualdisplay_driver_version_source "${_libvirtualdisplay_driver_version_source}")
    string(REGEX REPLACE "[-+].*$" "" _libvirtualdisplay_driver_version_source "${_libvirtualdisplay_driver_version_source}")
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(\\.([0-9]+))?$" _libvirtualdisplay_driver_version_match "${_libvirtualdisplay_driver_version_source}")
    if(NOT _libvirtualdisplay_driver_version_match)
        message(FATAL_ERROR "LIBVIRTUALDISPLAY_PACKAGE_VERSION must start with a numeric version usable for DriverVer, got '${LIBVIRTUALDISPLAY_PACKAGE_VERSION}'")
    endif()
    if("${CMAKE_MATCH_5}" STREQUAL "")
        set(_libvirtualdisplay_driver_version "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.0")
    else()
        set(_libvirtualdisplay_driver_version "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.${CMAKE_MATCH_5}")
    endif()
    set(LIBVIRTUALDISPLAY_DRIVER_VERSION "${_libvirtualdisplay_driver_version}" CACHE STRING "Four-part Windows DriverVer version for SunshineVirtualDisplayDriver.inf" FORCE)
    set(LIBVIRTUALDISPLAY_DRIVER_DATE "" CACHE STRING "Windows DriverVer date for SunshineVirtualDisplayDriver.inf, formatted MM/DD/YYYY")
    if(NOT LIBVIRTUALDISPLAY_DRIVER_DATE)
        string(TIMESTAMP LIBVIRTUALDISPLAY_DRIVER_DATE "%m/%d/%Y" UTC)
        set(LIBVIRTUALDISPLAY_DRIVER_DATE "${LIBVIRTUALDISPLAY_DRIVER_DATE}" CACHE STRING "Windows DriverVer date for SunshineVirtualDisplayDriver.inf, formatted MM/DD/YYYY" FORCE)
    endif()
    if(NOT LIBVIRTUALDISPLAY_DRIVER_DATE MATCHES "^[0-9][0-9]/[0-9][0-9]/[0-9][0-9][0-9][0-9]$")
        message(FATAL_ERROR "LIBVIRTUALDISPLAY_DRIVER_DATE must be formatted MM/DD/YYYY, got '${LIBVIRTUALDISPLAY_DRIVER_DATE}'")
    endif()

    set(SUNSHINE_DRIVER_INF_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/windows_driver/SunshineVirtualDisplayDriver.inf")
    add_custom_command(OUTPUT "${SUNSHINE_DRIVER_INF_GENERATED}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/windows_driver"
            COMMAND gen_driver_inf
                --driverver-date "${LIBVIRTUALDISPLAY_DRIVER_DATE}"
                --driverver-version "${LIBVIRTUALDISPLAY_DRIVER_VERSION}"
                "${SUNSHINE_DRIVER_INF_TEMPLATE}"
                "${SUNSHINE_DRIVER_INF_GENERATED}"
            DEPENDS gen_driver_inf "${SUNSHINE_DRIVER_INF_TEMPLATE}"
            COMMENT "Generating SunshineVirtualDisplayDriver.inf with DriverVer ${LIBVIRTUALDISPLAY_DRIVER_DATE},${LIBVIRTUALDISPLAY_DRIVER_VERSION}"
            VERBATIM)
    add_custom_target(gen_driver_package_inf DEPENDS "${SUNSHINE_DRIVER_INF_GENERATED}")

    set(SUNSHINE_REMOTE_DRIVER_INF_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/windows_driver/SunshineRemoteDisplayDriver.inf")
    add_custom_command(OUTPUT "${SUNSHINE_REMOTE_DRIVER_INF_GENERATED}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/windows_driver"
            COMMAND gen_driver_inf
                --driverver-date "${LIBVIRTUALDISPLAY_DRIVER_DATE}"
                --driverver-version "${LIBVIRTUALDISPLAY_DRIVER_VERSION}"
                "${SUNSHINE_REMOTE_DRIVER_INF_TEMPLATE}"
                "${SUNSHINE_REMOTE_DRIVER_INF_GENERATED}"
            DEPENDS gen_driver_inf "${SUNSHINE_REMOTE_DRIVER_INF_TEMPLATE}"
            COMMENT "Generating SunshineRemoteDisplayDriver.inf with DriverVer ${LIBVIRTUALDISPLAY_DRIVER_DATE},${LIBVIRTUALDISPLAY_DRIVER_VERSION}"
            VERBATIM)
    add_custom_target(gen_remote_driver_package_inf DEPENDS "${SUNSHINE_REMOTE_DRIVER_INF_GENERATED}")
endif()

if(BUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER OR BUILD_SUNSHINE_REMOTE_DISPLAY_DRIVER)
    add_subdirectory(windows_driver)
endif()

set(_libvirtualdisplay_tool_targets)

if(WIN32 AND BUILD_VIRTUALDISPLAY_PROBE)
    add_executable(virtualdisplay_probe "${PROJECT_SOURCE_DIR}/tools/virtualdisplay_probe.cpp")
    target_link_libraries(virtualdisplay_probe PRIVATE libvirtualdisplay::driver advapi32 d3d11 dxgi gdi32 mscms wtsapi32)
    if(MSVC)
      target_link_libraries(virtualdisplay_probe PRIVATE ole32 windowsapp)
    endif()
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

    # Alt-tab / teardown brute-force stress harness: creates its own temporary virtual
    # display, captures it like Sunshine (recreating the capture device on every mode
    # change), churns the mode + connect/disconnect, and watchdogs for hangs.
    add_executable(alttab_stress "${PROJECT_SOURCE_DIR}/tools/alttab_stress.cpp")
    target_link_libraries(alttab_stress PRIVATE libvirtualdisplay::driver advapi32 d3d11 dxgi)
    target_compile_features(alttab_stress PRIVATE cxx_std_20)
    install(TARGETS alttab_stress RUNTIME DESTINATION tools)
    list(APPEND _libvirtualdisplay_tool_targets alttab_stress)

    # Deterministic HDR10 producer used to prove what a terminal-session app submits
    # versus what the IddCx consumer receives after desktop composition.
    add_executable(hdr_session_probe "${PROJECT_SOURCE_DIR}/tools/hdr_session_probe.cpp")
    target_link_libraries(hdr_session_probe PRIVATE d3d11 dxgi gdi32 user32 ole32 windowsapp)
    target_compile_features(hdr_session_probe PRIVATE cxx_std_20)
    if(MINGW)
        target_link_options(hdr_session_probe PRIVATE -municode)
    endif()
    install(TARGETS hdr_session_probe RUNTIME DESTINATION tools)
    list(APPEND _libvirtualdisplay_tool_targets hdr_session_probe)
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
