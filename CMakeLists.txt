cmake_minimum_required(VERSION 3.24)

project(libvirtualdisplay
        VERSION 0.0.0
        DESCRIPTION "Clean-room virtual display driver support for Sunshine."
        HOMEPAGE_URL "https://github.com/Nonary/libvirtualdisplay"
        LANGUAGES CXX)

set(LIBVIRTUALDISPLAY_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "Version used for generated release packages")
set(LIBVIRTUALDISPLAY_LINUX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/linux" CACHE INTERNAL
        "Path to libvirtualdisplay Linux driver and service assets")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # Statically link the C/C++ runtime (/MT) instead of the default dynamic CRT (/MD).
    #
    # The UMDF/IddCx driver DLL is loaded by the WUDF host process. On a clean
    # end-user machine without the Visual C++ redistributable, a /MD driver
    # imports MSVCP140.dll / VCRUNTIME140.dll, which are absent, so the DLL fails
    # to load and the device reports "device cannot start" (Code 31). It works on
    # developer machines only because Visual Studio installs the redist there.
    #
    # Applied project-wide (relies on CMP0091 NEW, the default at our
    # cmake_minimum) so the static libvirtualdisplay_driver objects linked into
    # the driver DLL use the same CRT and don't trigger a defaultlib conflict.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    option(BUILD_TESTS "Build tests" ON)
endif()

add_subdirectory(src)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME AND BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(_libvirtualdisplay_license_file "${CMAKE_SOURCE_DIR}/LICENSE")
    install(CODE "
file(READ [[${_libvirtualdisplay_license_file}]] _libvirtualdisplay_license_text)
if(_libvirtualdisplay_license_text MATCHES [[No license, replace this file]])
    message(FATAL_ERROR [[LICENSE is still the template placeholder; replace it before packaging.]])
endif()
unset(_libvirtualdisplay_license_text)
")
    unset(_libvirtualdisplay_license_file)
    install(FILES README.md LICENSE DESTINATION .)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        install(DIRECTORY linux DESTINATION .)
    endif()

    set(CPACK_PACKAGE_NAME "libvirtualdisplay")
    set(CPACK_PACKAGE_VENDOR "LizardByte")
    set(CPACK_PACKAGE_CONTACT "https://github.com/Nonary/libvirtualdisplay")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
    set(CPACK_PACKAGE_VERSION "${LIBVIRTUALDISPLAY_PACKAGE_VERSION}")
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    if(WIN32)
        set(_libvirtualdisplay_package_system "windows")
        set(_libvirtualdisplay_package_arch "$ENV{VSCMD_ARG_TGT_ARCH}")
        if(NOT _libvirtualdisplay_package_arch)
            set(_libvirtualdisplay_package_arch "${CMAKE_VS_PLATFORM_NAME}")
        endif()
        if(NOT _libvirtualdisplay_package_arch)
            set(_libvirtualdisplay_package_arch "${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        string(TOLOWER "${_libvirtualdisplay_package_arch}" _libvirtualdisplay_package_arch)
        if(_libvirtualdisplay_package_arch MATCHES "^(amd64|x86_64)$")
            set(_libvirtualdisplay_package_arch "x64")
        elseif(_libvirtualdisplay_package_arch MATCHES "^(aarch64|arm64)$")
            set(_libvirtualdisplay_package_arch "arm64")
        elseif(_libvirtualdisplay_package_arch MATCHES "^(win32|x86|i[3-6]86)$")
            set(_libvirtualdisplay_package_arch "x86")
        elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_libvirtualdisplay_package_arch "x64")
        else()
            set(_libvirtualdisplay_package_arch "x86")
        endif()
    else()
        string(TOLOWER "${CMAKE_SYSTEM_NAME}" _libvirtualdisplay_package_system)
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _libvirtualdisplay_package_arch)
    endif()
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${_libvirtualdisplay_package_system}-${_libvirtualdisplay_package_arch}")
    unset(_libvirtualdisplay_package_arch)
    unset(_libvirtualdisplay_package_system)
    include(CPack)
endif()
