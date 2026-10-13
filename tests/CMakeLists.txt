cmake_minimum_required(VERSION 3.16)
project(controller-tests LANGUAGES CXX)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
enable_testing()
add_executable(gamepad-identity gamepad_identity.cpp)
target_compile_features(gamepad-identity PRIVATE cxx_std_11)
target_include_directories(gamepad-identity PRIVATE ../app ../moonlight-common-c/moonlight-common-c/src)
target_link_libraries(gamepad-identity PRIVATE PkgConfig::SDL2)
add_test(NAME gamepad-identity COMMAND gamepad-identity)

add_executable(gamepad-capabilities gamepad_capability_compat.cpp)
target_compile_features(gamepad-capabilities PRIVATE cxx_std_11)
target_include_directories(gamepad-capabilities PRIVATE ../app ../moonlight-common-c/moonlight-common-c/src)
add_test(NAME gamepad-capabilities COMMAND gamepad-capabilities)

add_executable(gamepad-feedback gamepad_feedback.cpp)
target_compile_features(gamepad-feedback PRIVATE cxx_std_11)
target_include_directories(gamepad-feedback PRIVATE ../app)
# Host-controlled slot indices: out-of-bounds reads must fail the test.
if(NOT MSVC)
    target_compile_options(gamepad-feedback PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    target_link_options(gamepad-feedback PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME gamepad-feedback COMMAND gamepad-feedback)

pkg_check_modules(PLACEBO REQUIRED IMPORTED_TARGET libplacebo)
add_executable(pyrowave-color pyrowave_color.cpp)
target_compile_features(pyrowave-color PRIVATE cxx_std_11)
target_include_directories(pyrowave-color PRIVATE ../app)
target_link_libraries(pyrowave-color PRIVATE PkgConfig::PLACEBO)
# assert() is the test oracle, including in Release builds.
target_compile_options(pyrowave-color PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
add_test(NAME pyrowave-color COMMAND pyrowave-color)

foreach(frame_test IN ITEMS pyrowave-frame pyrowave-frame-winmacros)
    add_executable(${frame_test} pyrowave_frame.cpp)
    target_compile_features(${frame_test} PRIVATE cxx_std_11)
    target_include_directories(${frame_test} PRIVATE ../app)
    target_compile_options(${frame_test} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
    if(frame_test STREQUAL "pyrowave-frame-winmacros")
        target_compile_definitions(${frame_test} PRIVATE TEST_WINDOWS_MINMAX)
    endif()
    add_test(NAME ${frame_test} COMMAND ${frame_test})
endforeach()

add_executable(display-backend display_backend.cpp)
target_compile_features(display-backend PRIVATE cxx_std_11)
target_compile_options(display-backend PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
add_test(NAME display-backend COMMAND display-backend)

add_executable(pyrowave-records pyrowave_records.cpp ../app/streaming/video/pyrowaveframing.cpp)
target_compile_features(pyrowave-records PRIVATE cxx_std_17)
add_test(NAME pyrowave-records COMMAND pyrowave-records)
add_executable(pyrowave-bandwidth pyrowave_bandwidth.cpp)
target_compile_features(pyrowave-bandwidth PRIVATE cxx_std_17)
target_compile_options(pyrowave-bandwidth PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
add_test(NAME pyrowave-bandwidth COMMAND pyrowave-bandwidth)

# Optional runtime HTTPS fixture harness uses the production downloader.
find_package(Qt5 COMPONENTS Core Network Gui QUIET)
find_package(OpenSSL QUIET)
find_library(MOONLIGHT_PROTOCOL_LIBRARY NAMES moonlight-common-c)
if(Qt5_FOUND AND OpenSSL_FOUND AND MOONLIGHT_PROTOCOL_LIBRARY)
    add_executable(pyrowave-https pyrowave_https.cpp ../app/backend/nvhttp.cpp
        ../app/backend/identitymanager.cpp ../app/backend/nvaddress.cpp ../app/backend/nvapp.cpp)
    set_target_properties(pyrowave-https PROPERTIES AUTOMOC ON)
    target_compile_features(pyrowave-https PRIVATE cxx_std_17)
    target_include_directories(pyrowave-https PRIVATE ../app ../moonlight-common-c/moonlight-common-c/src)
    target_link_libraries(pyrowave-https PRIVATE Qt5::Core Qt5::Network Qt5::Gui OpenSSL::SSL OpenSSL::Crypto
        PkgConfig::SDL2 ${MOONLIGHT_PROTOCOL_LIBRARY})
endif()

add_executable(pyrowave-golden pyrowave_golden.cpp ../app/streaming/video/pyrowaveframing.cpp)
target_compile_features(pyrowave-golden PRIVATE cxx_std_17)
target_include_directories(pyrowave-golden PRIVATE ../app)
target_compile_options(pyrowave-golden PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
add_test(NAME pyrowave-golden COMMAND pyrowave-golden ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

add_executable(vrr-timing vrr_timing.cpp
    ../app/streaming/vrrratepolicy.cpp
    ../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrsourceclock.cpp
    ../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.cpp)
target_compile_features(vrr-timing PRIVATE cxx_std_17)
target_include_directories(vrr-timing PRIVATE ../app)
target_compile_options(vrr-timing PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG -Wall -Wextra>)
add_test(NAME vrr-timing COMMAND vrr-timing)

find_package(Threads REQUIRED)
add_executable(vrr-worker vrr_worker.cpp
    ../app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.cpp
    ../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrsourceclock.cpp
    ../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.cpp)
target_compile_features(vrr-worker PRIVATE cxx_std_17)
target_include_directories(vrr-worker PRIVATE ../app)
target_link_libraries(vrr-worker PRIVATE Threads::Threads)
target_compile_options(vrr-worker PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG -Wall -Wextra>)
add_test(NAME vrr-worker COMMAND vrr-worker)
