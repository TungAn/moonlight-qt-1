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

add_executable(plvk-color-range plvk_color_range.cpp)
target_compile_features(plvk-color-range PRIVATE cxx_std_11)
target_include_directories(plvk-color-range PRIVATE ../app)
target_link_libraries(plvk-color-range PRIVATE PkgConfig::PLACEBO)
target_compile_options(plvk-color-range PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
add_test(NAME plvk-color-range COMMAND plvk-color-range)

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

add_executable(controller-input controller_input.cpp)
target_compile_features(controller-input PRIVATE cxx_std_11)
target_include_directories(controller-input PRIVATE ../app)
target_compile_options(controller-input PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
add_test(NAME controller-input COMMAND controller-input)

# Exercise the production SDL handlers and intercept the actual outbound calls.
# No live host, controller hardware, Qt event loop or display server is needed.
find_package(Qt6 COMPONENTS Core Gui QUIET)
if(UNIX AND Qt6_FOUND AND SDL2_VERSION VERSION_GREATER_EQUAL "2.0.14")
    add_executable(controller-input-sdl controller_input_sdl.cpp
        ../app/streaming/input/input.cpp ../app/streaming/input/gamepad.cpp
        ../app/streaming/input/gamescopeinputfocus.cpp)
    target_compile_features(controller-input-sdl PRIVATE cxx_std_17)
    target_include_directories(controller-input-sdl PRIVATE input-stubs ../app
        ../moonlight-common-c/moonlight-common-c/src)
    target_link_libraries(controller-input-sdl PRIVATE Qt6::Core Qt6::Gui PkgConfig::SDL2)
    target_compile_options(controller-input-sdl PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
    add_test(NAME controller-input-sdl COMMAND controller-input-sdl)
    set_tests_properties(controller-input-sdl PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(GAMESCOPE_XCB IMPORTED_TARGET xcb xau)
    if(GAMESCOPE_XCB_FOUND AND TARGET controller-input-sdl)
        target_compile_definitions(controller-input-sdl PRIVATE HAS_GAMESCOPE_INPUT)
        target_link_libraries(controller-input-sdl PRIVATE PkgConfig::GAMESCOPE_XCB Threads::Threads)
        add_executable(gamescope-input-focus gamescope_input_focus.cpp controller_input_sdl.cpp
            ../app/streaming/input/input.cpp ../app/streaming/input/gamepad.cpp
            ../app/streaming/input/gamescopeinputfocus.cpp)
        target_compile_features(gamescope-input-focus PRIVATE cxx_std_17)
        target_include_directories(gamescope-input-focus PRIVATE input-stubs ../app
            ../moonlight-common-c/moonlight-common-c/src)
        target_compile_definitions(gamescope-input-focus PRIVATE HAS_GAMESCOPE_INPUT CONTROLLER_INPUT_FIXTURE_ONLY)
        target_link_libraries(gamescope-input-focus PRIVATE Qt6::Core Qt6::Gui PkgConfig::GAMESCOPE_XCB PkgConfig::SDL2 Threads::Threads)
        target_compile_options(gamescope-input-focus PRIVATE -UNDEBUG)
        add_executable(gamepad-ui-navigation gamepad_ui_navigation.cpp
            ../app/gui/sdlgamepadkeynavigation.cpp
            ../app/streaming/input/gamescopeinputfocus.cpp)
        set_target_properties(gamepad-ui-navigation PROPERTIES AUTOMOC ON)
        target_compile_features(gamepad-ui-navigation PRIVATE cxx_std_17)
        target_include_directories(gamepad-ui-navigation PRIVATE input-stubs ../app)
        target_compile_definitions(gamepad-ui-navigation PRIVATE HAS_GAMESCOPE_INPUT)
        target_link_libraries(gamepad-ui-navigation PRIVATE Qt6::Core Qt6::Gui
            PkgConfig::GAMESCOPE_XCB PkgConfig::SDL2 Threads::Threads)
        target_compile_options(gamepad-ui-navigation PRIVATE -UNDEBUG)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        find_program(XVFB_EXECUTABLE Xvfb)
        if(Python3_FOUND AND XVFB_EXECUTABLE)
            add_test(NAME gamescope-input-focus COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/gamescope_input_focus.py ${XVFB_EXECUTABLE}
                $<TARGET_FILE:gamescope-input-focus>)
            set_tests_properties(gamescope-input-focus PROPERTIES TIMEOUT 40)
            add_test(NAME gamepad-ui-navigation COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/gamescope_input_focus.py ${XVFB_EXECUTABLE}
                $<TARGET_FILE:gamepad-ui-navigation>)
            set_tests_properties(gamepad-ui-navigation PROPERTIES TIMEOUT 40)
        endif()
    endif()
endif()

add_executable(stream-switch stream_switch.cpp)
target_include_directories(stream-switch PRIVATE ../app)
target_compile_features(stream-switch PRIVATE cxx_std_11)
target_compile_options(stream-switch PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
add_test(NAME stream-switch COMMAND stream-switch)
