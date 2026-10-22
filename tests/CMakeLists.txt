include(GoogleTest)

set(INSTALL_GTEST OFF)
set(INSTALL_GMOCK OFF)
set(gtest_force_shared_crt ON CACHE BOOL "Always use msvcrt.dll" FORCE)
set(LIBVIRTUALDISPLAY_GOOGLETEST_SOURCE_DIR "" CACHE PATH "Optional local googletest source directory")

if(LIBVIRTUALDISPLAY_GOOGLETEST_SOURCE_DIR AND EXISTS "${LIBVIRTUALDISPLAY_GOOGLETEST_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${LIBVIRTUALDISPLAY_GOOGLETEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")
else()
    include(FetchContent)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723) # v1.17.0
    FetchContent_MakeAvailable(googletest)
endif()

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.h")

add_executable(test_libvirtualdisplay ${TEST_SOURCES})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_include_directories(test_libvirtualdisplay PRIVATE
            "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm")
endif()
target_link_libraries(test_libvirtualdisplay
        PRIVATE
        gmock_main
        libvirtualdisplay::driver)
if(TARGET gen_driver_inf)
    add_dependencies(test_libvirtualdisplay gen_driver_inf)
endif()

gtest_discover_tests(test_libvirtualdisplay)

# The driver INF is a generated artifact: its security-critical values come from the
# device_identity source of truth via gen_driver_inf. Verify the committed INF stays in
# sync instead of scraping its text from a unit test.
add_test(NAME driver_inf_in_sync
        COMMAND gen_driver_inf --check
            "${PROJECT_SOURCE_DIR}/src/driver/windows_driver/SunshineVirtualDisplayDriver.inf.in"
            "${PROJECT_SOURCE_DIR}/src/driver/windows_driver/SunshineVirtualDisplayDriver.inf")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_kwin_native_abi "${PROJECT_SOURCE_DIR}/tests/linux/test-kwin-native-abi.cpp")
    target_link_libraries(test_kwin_native_abi PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME linux_kwin_native_abi COMMAND test_kwin_native_abi)
    if(TARGET vibeshine_kwin_gpu)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBVIRTUALDISPLAY_TEST_KWIN REQUIRED IMPORTED_TARGET libdrm gbm)
        add_executable(test_kwin_gpu_routing "${PROJECT_SOURCE_DIR}/tests/linux/test-kwin-gpu-routing.cpp")
        target_link_libraries(test_kwin_gpu_routing PRIVATE
                PkgConfig::LIBVIRTUALDISPLAY_TEST_KWIN ${CMAKE_DL_LIBS})
        add_test(NAME linux_kwin_gpu_routing COMMAND test_kwin_gpu_routing)
        get_target_property(_kwin_bridge_definitions vibeshine_kwin_gpu COMPILE_DEFINITIONS)
        if("${_kwin_bridge_definitions}" MATCHES "VIBESHINE_KWIN_NATIVE_ALLOCATOR=1")
            find_package(Qt6 COMPONENTS Core REQUIRED)
            find_package(Threads REQUIRED)
            add_executable(test_kwin_gpu_routing_native "${PROJECT_SOURCE_DIR}/tests/linux/test-kwin-gpu-routing.cpp")
            target_include_directories(test_kwin_gpu_routing_native PRIVATE "${LIBVIRTUALDISPLAY_KWIN_INCLUDE_DIR}")
            target_compile_definitions(test_kwin_gpu_routing_native PRIVATE
                    ${_kwin_bridge_definitions})
            target_link_libraries(test_kwin_gpu_routing_native PRIVATE
                    PkgConfig::LIBVIRTUALDISPLAY_TEST_KWIN
                    Qt6::Core "${LIBVIRTUALDISPLAY_KWIN_LIBRARY}" Threads::Threads ${CMAKE_DL_LIBS})
            add_test(NAME linux_kwin_gpu_routing_native COMMAND test_kwin_gpu_routing_native)
            pkg_check_modules(LIBVIRTUALDISPLAY_TEST_EGL REQUIRED IMPORTED_TARGET egl epoxy)
            add_executable(test_kwin_native_allocation "${PROJECT_SOURCE_DIR}/tests/linux/test-kwin-native-allocation.cpp")
            set_target_properties(test_kwin_native_allocation PROPERTIES ENABLE_EXPORTS ON)
            target_include_directories(test_kwin_native_allocation PRIVATE "${LIBVIRTUALDISPLAY_KWIN_INCLUDE_DIR}")
            target_compile_definitions(test_kwin_native_allocation PRIVATE ${_kwin_bridge_definitions})
            target_link_libraries(test_kwin_native_allocation PRIVATE
                    PkgConfig::LIBVIRTUALDISPLAY_TEST_KWIN PkgConfig::LIBVIRTUALDISPLAY_TEST_EGL
                    Qt6::Core "${LIBVIRTUALDISPLAY_KWIN_LIBRARY}" ${CMAKE_DL_LIBS})
            add_test(NAME linux_kwin_native_allocation COMMAND test_kwin_native_allocation)
            set_tests_properties(linux_kwin_native_allocation PROPERTIES SKIP_RETURN_CODE 77 LABELS "integration;gpu")
        endif()
    endif()
    find_program(LIBVIRTUALDISPLAY_BASH_EXECUTABLE bash)
    if(LIBVIRTUALDISPLAY_BASH_EXECUTABLE)
        add_test(NAME linux_vkms_setup
                COMMAND "${LIBVIRTUALDISPLAY_BASH_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/linux/test-vibeshine-vkms.sh"
                    "${LIBVIRTUALDISPLAY_LINUX_ROOT}/packaging/vibeshine-vkms")
        add_test(NAME linux_drm_install
                COMMAND "${LIBVIRTUALDISPLAY_BASH_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/linux/test-vibeshine-drm-install.sh"
                    "${LIBVIRTUALDISPLAY_LINUX_ROOT}/packaging/vibeshine-drm-install.in")
        add_test(NAME linux_drm_build_module
                COMMAND "${LIBVIRTUALDISPLAY_BASH_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/linux/test-vibeshine-drm-build-module.sh"
                    "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm/build-module"
                    "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm/dkms.conf.in")
    endif()

    find_program(LIBVIRTUALDISPLAY_PYTHON_EXECUTABLE NAMES python3)
    if(LIBVIRTUALDISPLAY_PYTHON_EXECUTABLE)
        # The CTest command invokes this helper by its target-file path. Ensure
        # it is included when CI builds only test_libvirtualdisplay.
        if(TARGET vibeshine_vkms_peercred)
            add_dependencies(test_libvirtualdisplay vibeshine_vkms_peercred)
        endif()

        add_test(NAME linux_drm_native_allocation
                COMMAND "${LIBVIRTUALDISPLAY_PYTHON_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/linux/test-native-dumb-allocation.py"
                    "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm")
        add_test(NAME linux_vkms_peercred
                COMMAND "${LIBVIRTUALDISPLAY_PYTHON_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/linux/test-vkms-peercred.py"
                    "$<TARGET_FILE:vibeshine_vkms_peercred>")
        add_test(NAME linux_drm_contract
                COMMAND "${LIBVIRTUALDISPLAY_PYTHON_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/linux/test-vibeshine-drm-contract.py"
                    "${LIBVIRTUALDISPLAY_LINUX_ROOT}/vibeshine-drm")
        add_test(NAME linux_kwin_gpu_bridge_contract
                COMMAND "${LIBVIRTUALDISPLAY_PYTHON_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/linux/test-kwin-gpu-bridge-contract.py"
                    "${LIBVIRTUALDISPLAY_LINUX_ROOT}")
    endif()
endif()
