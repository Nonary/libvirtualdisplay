if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "BUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER requires a Windows MSVC/WDK build environment")
endif()

set(SUNSHINE_DRIVER_TARGET SunshineVirtualDisplayDriver)
set(SUNSHINE_DRIVER_WDF_VERSION "2.25" CACHE STRING "UMDF version for the Sunshine virtual display driver" FORCE)
set(SUNSHINE_DRIVER_IDDCX_VERSION "1.10" CACHE STRING "IddCx include/library version for the Sunshine virtual display driver" FORCE)
set(SUNSHINE_DRIVER_PLATFORM "${CMAKE_VS_PLATFORM_NAME}")
if(SUNSHINE_DRIVER_INF_GENERATED)
    set(SUNSHINE_DRIVER_INF "${SUNSHINE_DRIVER_INF_GENERATED}")
else()
    set(SUNSHINE_DRIVER_INF "${CMAKE_CURRENT_SOURCE_DIR}/SunshineVirtualDisplayDriver.inf")
endif()

if(NOT SUNSHINE_DRIVER_PLATFORM)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(SUNSHINE_DRIVER_PLATFORM "x64")
    else()
        set(SUNSHINE_DRIVER_PLATFORM "x86")
    endif()
endif()

if(SUNSHINE_DRIVER_PLATFORM MATCHES "^[Xx]64$|^[Aa][Mm][Dd]64$")
    set(SUNSHINE_DRIVER_WDK_ARCH "x64")
    set(SUNSHINE_DRIVER_INF2CAT_OS "10_X64")
else()
    message(FATAL_ERROR "Sunshine virtual display driver packaging currently supports only x64/amd64; got ${SUNSHINE_DRIVER_PLATFORM}")
endif()

set(_SUNSHINE_DRIVER_DEFAULT_WDK_ROOT "C:/Program Files (x86)/Windows Kits/10")

function(_sunshine_driver_sdk_is_usable result root version)
    set(_usable FALSE)
    if(root AND version)
        string(REGEX REPLACE "\\\\$" "" _root "${root}")
        string(REGEX REPLACE "\\\\$" "" _version "${version}")
        if(EXISTS "${_root}/Include/wdf/umdf/${SUNSHINE_DRIVER_WDF_VERSION}" AND
                EXISTS "${_root}/Lib/wdf/umdf/${SUNSHINE_DRIVER_WDK_ARCH}/${SUNSHINE_DRIVER_WDF_VERSION}/WdfDriverStubUm.lib" AND
                EXISTS "${_root}/Lib/${_version}/um/${SUNSHINE_DRIVER_WDK_ARCH}/iddcx/${SUNSHINE_DRIVER_IDDCX_VERSION}/IddCxStub.lib")
            set(_usable TRUE)
        endif()
    endif()
    set(${result} "${_usable}" PARENT_SCOPE)
endfunction()

function(_sunshine_driver_find_sdk_version result root)
    set(_resolved_version "")
    if(root AND EXISTS "${root}/Lib")
        file(GLOB _sdk_dirs LIST_DIRECTORIES true "${root}/Lib/[0-9]*")
        set(_sdk_versions)
        foreach(_sdk_dir IN LISTS _sdk_dirs)
            get_filename_component(_sdk_version "${_sdk_dir}" NAME)
            if(_sdk_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
                list(APPEND _sdk_versions "${_sdk_version}")
            endif()
        endforeach()
        list(SORT _sdk_versions COMPARE NATURAL ORDER DESCENDING)
        foreach(_sdk_version IN LISTS _sdk_versions)
            _sunshine_driver_sdk_is_usable(_usable "${root}" "${_sdk_version}")
            if(_usable)
                set(_resolved_version "${_sdk_version}")
                break()
            endif()
        endforeach()
    endif()
    set(${result} "${_resolved_version}" PARENT_SCOPE)
endfunction()

set(_sunshine_driver_candidate_roots)
if(SUNSHINE_DRIVER_WDK_ROOT)
    list(APPEND _sunshine_driver_candidate_roots "${SUNSHINE_DRIVER_WDK_ROOT}")
endif()
if("$ENV{WindowsSdkDir}")
    list(APPEND _sunshine_driver_candidate_roots "$ENV{WindowsSdkDir}")
endif()
list(APPEND _sunshine_driver_candidate_roots "${_SUNSHINE_DRIVER_DEFAULT_WDK_ROOT}")
list(REMOVE_DUPLICATES _sunshine_driver_candidate_roots)

set(_sunshine_driver_preferred_sdk_version "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
if(NOT _sunshine_driver_preferred_sdk_version)
    set(_sunshine_driver_preferred_sdk_version "$ENV{WindowsSDKVersion}")
endif()
string(REGEX REPLACE "\\\\$" "" _sunshine_driver_preferred_sdk_version "${_sunshine_driver_preferred_sdk_version}")

set(SUNSHINE_DRIVER_SDK_VERSION "")
foreach(_sunshine_driver_candidate_root IN LISTS _sunshine_driver_candidate_roots)
    cmake_path(ABSOLUTE_PATH _sunshine_driver_candidate_root NORMALIZE OUTPUT_VARIABLE _sunshine_driver_candidate_root)
    if(_sunshine_driver_preferred_sdk_version)
        _sunshine_driver_sdk_is_usable(_sunshine_driver_preferred_usable "${_sunshine_driver_candidate_root}" "${_sunshine_driver_preferred_sdk_version}")
        if(_sunshine_driver_preferred_usable)
            set(SUNSHINE_DRIVER_WDK_ROOT "${_sunshine_driver_candidate_root}" CACHE PATH "Windows Kits root used for the Sunshine virtual display driver" FORCE)
            set(SUNSHINE_DRIVER_SDK_VERSION "${_sunshine_driver_preferred_sdk_version}")
            break()
        endif()
    endif()

    _sunshine_driver_find_sdk_version(_sunshine_driver_resolved_sdk_version "${_sunshine_driver_candidate_root}")
    if(_sunshine_driver_resolved_sdk_version)
        set(SUNSHINE_DRIVER_WDK_ROOT "${_sunshine_driver_candidate_root}" CACHE PATH "Windows Kits root used for the Sunshine virtual display driver" FORCE)
        set(SUNSHINE_DRIVER_SDK_VERSION "${_sunshine_driver_resolved_sdk_version}")
        break()
    endif()
endforeach()

if(NOT SUNSHINE_DRIVER_SDK_VERSION)
    message(FATAL_ERROR "Unable to find a WDK/SDK version with required WDF and IddCx files for ${SUNSHINE_DRIVER_TARGET}")
endif()

if(NOT SUNSHINE_DRIVER_WDK_ROOT)
    message(FATAL_ERROR "SUNSHINE_DRIVER_WDK_ROOT is required to build ${SUNSHINE_DRIVER_TARGET}")
endif()

cmake_path(ABSOLUTE_PATH SUNSHINE_DRIVER_WDK_ROOT NORMALIZE OUTPUT_VARIABLE SUNSHINE_DRIVER_WDK_ROOT)
set(_SUNSHINE_DRIVER_IDDCX_LIB "${SUNSHINE_DRIVER_WDK_ROOT}/Lib/${SUNSHINE_DRIVER_SDK_VERSION}/um/${SUNSHINE_DRIVER_WDK_ARCH}/iddcx/${SUNSHINE_DRIVER_IDDCX_VERSION}/IddCxStub.lib")
if(NOT EXISTS "${_SUNSHINE_DRIVER_IDDCX_LIB}")
    message(FATAL_ERROR "Unable to find IddCxStub.lib under SUNSHINE_DRIVER_WDK_ROOT=${SUNSHINE_DRIVER_WDK_ROOT}")
endif()
unset(_SUNSHINE_DRIVER_DEFAULT_WDK_ROOT)
unset(_SUNSHINE_DRIVER_IDDCX_LIB)
unset(_sunshine_driver_candidate_roots)
unset(_sunshine_driver_preferred_sdk_version)
unset(_sunshine_driver_preferred_usable)
unset(_sunshine_driver_resolved_sdk_version)
unset(_sunshine_driver_candidate_root)

add_library(${SUNSHINE_DRIVER_TARGET} SHARED
        driver_main.cpp
        "${SUNSHINE_DRIVER_INF}")
if(TARGET gen_driver_package_inf)
    add_dependencies(${SUNSHINE_DRIVER_TARGET} gen_driver_package_inf)
endif()

find_program(SUNSHINE_DRIVER_TRACEWPP
        NAMES TraceWPP.exe tracewpp.exe
        PATHS
        "${SUNSHINE_DRIVER_WDK_ROOT}/bin/${SUNSHINE_DRIVER_SDK_VERSION}/x64"
        "${SUNSHINE_DRIVER_WDK_ROOT}/bin/${SUNSHINE_DRIVER_SDK_VERSION}/x86"
        NO_DEFAULT_PATH)

if(NOT SUNSHINE_DRIVER_TRACEWPP)
    message(FATAL_ERROR "TraceWPP.exe is required to build ${SUNSHINE_DRIVER_TARGET} WPP/IFR tracing")
endif()

set(SUNSHINE_DRIVER_WPP_TMH "${CMAKE_CURRENT_BINARY_DIR}/driver_main.tmh")
set(SUNSHINE_DRIVER_WPP_CONFIG "${SUNSHINE_DRIVER_WDK_ROOT}/bin/${SUNSHINE_DRIVER_SDK_VERSION}/WppConfig/Rev1")
add_custom_command(OUTPUT "${SUNSHINE_DRIVER_WPP_TMH}"
        COMMAND "${SUNSHINE_DRIVER_TRACEWPP}"
        -um
        -dll
        -DWPP_MACRO_USE_KM_VERSION_FOR_UM=1
        -DENABLE_WPP_RECORDER=1
        "-I${SUNSHINE_DRIVER_WPP_CONFIG}"
        "-func:TraceEvents(LEVEL,FLAGS,MSG,...)"
        "-oDir:${CMAKE_CURRENT_BINARY_DIR}"
        driver_main.cpp
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/driver_main.cpp"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        VERBATIM)
add_custom_target(${SUNSHINE_DRIVER_TARGET}WppTrace DEPENDS "${SUNSHINE_DRIVER_WPP_TMH}")
add_dependencies(${SUNSHINE_DRIVER_TARGET} ${SUNSHINE_DRIVER_TARGET}WppTrace)

target_link_libraries(${SUNSHINE_DRIVER_TARGET}
        PRIVATE
        libvirtualdisplay::driver
        advapi32
        avrt
        d3d11
        dxgi
        wtsapi32
        "${SUNSHINE_DRIVER_WDK_ROOT}/Lib/${SUNSHINE_DRIVER_SDK_VERSION}/um/${SUNSHINE_DRIVER_WDK_ARCH}/iddcx/${SUNSHINE_DRIVER_IDDCX_VERSION}/IddCxStub.lib"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Lib/wdf/umdf/${SUNSHINE_DRIVER_WDK_ARCH}/${SUNSHINE_DRIVER_WDF_VERSION}/WdfDriverStubUm.lib"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Lib/${SUNSHINE_DRIVER_SDK_VERSION}/um/${SUNSHINE_DRIVER_WDK_ARCH}/wpprecorderum.lib"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Lib/${SUNSHINE_DRIVER_SDK_VERSION}/um/${SUNSHINE_DRIVER_WDK_ARCH}/ntdll.lib")

target_include_directories(${SUNSHINE_DRIVER_TARGET}
        PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Include/wdf/umdf/${SUNSHINE_DRIVER_WDF_VERSION}"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Include/${SUNSHINE_DRIVER_SDK_VERSION}/um"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Include/${SUNSHINE_DRIVER_SDK_VERSION}/shared"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Include/${SUNSHINE_DRIVER_SDK_VERSION}/ucrt"
        "${SUNSHINE_DRIVER_WDK_ROOT}/Include/${SUNSHINE_DRIVER_SDK_VERSION}/um/iddcx/${SUNSHINE_DRIVER_IDDCX_VERSION}")

target_compile_definitions(${SUNSHINE_DRIVER_TARGET}
        PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        UMDF_DRIVER
        UMDF_VERSION_MAJOR=2
        UMDF_VERSION_MINOR=25
        UMDF_MINIMUM_VERSION_REQUIRED=25
        _ATL_NO_WIN_SUPPORT
        IDDCX_VERSION_MAJOR=1
        IDDCX_VERSION_MINOR=10
        IDDCX_MINIMUM_VERSION_REQUIRED=4
        WPP_MACRO_USE_KM_VERSION_FOR_UM=1
        ENABLE_WPP_RECORDER=1)

set_target_properties(${SUNSHINE_DRIVER_TARGET}
        PROPERTIES
        PREFIX ""
        OUTPUT_NAME "SunshineVirtualDisplayDriver"
        SUFFIX ".dll")

# Keep the package inputs as real build outputs. A POST_BUILD-only copy does not
# run when only the INF changes, which can leave pnputil installing a stale driver package.
set(SUNSHINE_DRIVER_PACKAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/driver-package")
set(SUNSHINE_DRIVER_PACKAGED_INF "${SUNSHINE_DRIVER_PACKAGE_DIR}/SunshineVirtualDisplayDriver.inf")
set(SUNSHINE_DRIVER_PACKAGED_DLL "${SUNSHINE_DRIVER_PACKAGE_DIR}/SunshineVirtualDisplayDriver.dll")
add_custom_command(OUTPUT "${SUNSHINE_DRIVER_PACKAGED_INF}" "${SUNSHINE_DRIVER_PACKAGED_DLL}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SUNSHINE_DRIVER_PACKAGE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SUNSHINE_DRIVER_INF}"
                "${SUNSHINE_DRIVER_PACKAGED_INF}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${SUNSHINE_DRIVER_TARGET}>"
                "${SUNSHINE_DRIVER_PACKAGED_DLL}"
        DEPENDS ${SUNSHINE_DRIVER_TARGET} "${SUNSHINE_DRIVER_INF}"
        VERBATIM)

find_program(SUNSHINE_DRIVER_INF2CAT
        NAMES Inf2Cat.exe
        PATHS
        "${SUNSHINE_DRIVER_WDK_ROOT}/bin/${SUNSHINE_DRIVER_SDK_VERSION}/x86"
        "${SUNSHINE_DRIVER_WDK_ROOT}/bin/${SUNSHINE_DRIVER_SDK_VERSION}/x64"
        NO_DEFAULT_PATH)

if(NOT SUNSHINE_DRIVER_INF2CAT)
    message(FATAL_ERROR "Inf2Cat.exe is required to package ${SUNSHINE_DRIVER_TARGET}")
endif()

set(SUNSHINE_DRIVER_PACKAGED_CAT "${SUNSHINE_DRIVER_PACKAGE_DIR}/sunshinevirtualdisplaydriver.cat")
add_custom_command(OUTPUT "${SUNSHINE_DRIVER_PACKAGED_CAT}"
        COMMAND "${SUNSHINE_DRIVER_INF2CAT}" "/driver:${SUNSHINE_DRIVER_PACKAGE_DIR}" /os:${SUNSHINE_DRIVER_INF2CAT_OS}
        DEPENDS "${SUNSHINE_DRIVER_PACKAGED_INF}" "${SUNSHINE_DRIVER_PACKAGED_DLL}"
        VERBATIM)
add_custom_target(${SUNSHINE_DRIVER_TARGET}PackageFiles ALL DEPENDS "${SUNSHINE_DRIVER_PACKAGED_CAT}")

install(TARGETS ${SUNSHINE_DRIVER_TARGET} RUNTIME DESTINATION driver)
install(FILES "$<TARGET_PDB_FILE:${SUNSHINE_DRIVER_TARGET}>" DESTINATION driver OPTIONAL)
install(FILES "${SUNSHINE_DRIVER_PACKAGED_INF}" DESTINATION driver)
install(FILES "${SUNSHINE_DRIVER_PACKAGED_CAT}" DESTINATION driver RENAME SunshineVirtualDisplayDriver.cat)
