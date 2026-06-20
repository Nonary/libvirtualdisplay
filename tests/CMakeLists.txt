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
