cmake_minimum_required(VERSION 3.21)
project(Fulskrin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(MSVC)
  add_compile_options(/utf-8 /W3)   # UTF-8 sources (UI strings contain non-ASCII characters)
  add_compile_definitions(NOMINMAX _USE_MATH_DEFINES)
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network)

# ---------------------------------------------------------------------------
# FFmpeg: pkg-config (macOS Homebrew, Linux) or FFMPEG_ROOT (Windows, "shared" build)
# ---------------------------------------------------------------------------
set(FFMPEG_ROOT "" CACHE PATH "FFmpeg folder containing include/ and lib/ (Windows)")
set(FFMPEG_LIBS avformat avcodec swscale swresample avutil)

if(FFMPEG_ROOT)
  add_library(ffmpeg INTERFACE)
  target_include_directories(ffmpeg INTERFACE "${FFMPEG_ROOT}/include")
  foreach(lib ${FFMPEG_LIBS})
    find_library(FF_${lib} NAMES ${lib} lib${lib} PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(ffmpeg INTERFACE ${FF_${lib}})
  endforeach()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)
  add_library(ffmpeg INTERFACE)
  target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

# ---------------------------------------------------------------------------
# Engine: no dependency on widgets (reusable with another UI)
# ---------------------------------------------------------------------------
add_library(fulskrin_engine STATIC
  src/engine/Gl.cpp
  src/engine/Isf.cpp
  src/engine/IsfLibrary.cpp
  src/engine/Mapping.cpp
  src/engine/VideoDecoder.cpp
  src/engine/Engine.cpp
  src/engine/Publish.cpp
  src/engine/AudioStream.cpp
  src/engine/AudioOutput.cpp
  src/engine/MiniaudioImpl.cpp
  src/engine/Osc.cpp
  src/engine/Zeroconf.cpp
  src/engine/Memories.cpp
)
target_include_directories(fulskrin_engine PUBLIC src/engine)
target_link_libraries(fulskrin_engine PUBLIC Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::Network ffmpeg)
target_compile_definitions(fulskrin_engine PUBLIC GL_SILENCE_DEPRECATION)

# ---------------------------------------------------------------------------
# Audio output: miniaudio (single header, public domain / MIT-0), downloaded at configure time
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 9634bedb5b5a2ca38c1ee7108a9358a4e233f14d # 0.11.25
  GIT_SHALLOW FALSE
  SOURCE_SUBDIR no-cmake) # download only: compiled in src/engine/MiniaudioImpl.cpp
FetchContent_MakeAvailable(miniaudio)
target_include_directories(fulskrin_engine PRIVATE "${miniaudio_SOURCE_DIR}")
target_compile_definitions(fulskrin_engine PRIVATE MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_ENGINE
  MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH)
if(MSVC)
  set_source_files_properties(src/engine/MiniaudioImpl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
else()
  set_source_files_properties(src/engine/MiniaudioImpl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
if(APPLE)
  # Link the Core Audio frameworks directly rather than loading them at runtime
  target_compile_definitions(fulskrin_engine PRIVATE MA_NO_RUNTIME_LINKING)
  target_link_libraries(fulskrin_engine PRIVATE "-framework CoreFoundation" "-framework CoreAudio"
    "-framework AudioToolbox")
elseif(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(fulskrin_engine PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# ---------------------------------------------------------------------------
# Output publishing
#  - NDI and OMT: loaded at runtime (no build-time dependency)
#  - Syphon (macOS) and Spout (Windows): sources downloaded and built with Fulskrin
# ---------------------------------------------------------------------------
option(FULSKRIN_SYPHON "Syphon publishing (macOS)" ON)
option(FULSKRIN_SPOUT "Spout publishing (Windows)" ON)

if(APPLE AND FULSKRIN_SYPHON)
  enable_language(C OBJC OBJCXX) # Syphon: C and Objective-C sources
  FetchContent_Declare(syphon
    GIT_REPOSITORY https://github.com/Syphon/Syphon-Framework.git
    GIT_TAG f4761677a45b8034a3c2069ec0f3d2553da81fba
    SOURCE_SUBDIR no-cmake) # download only: we build the sources ourselves
  FetchContent_MakeAvailable(syphon)
  # The sources import <Syphon/...>: expose the headers under this prefix.
  file(GLOB SYPHON_HEADERS "${syphon_SOURCE_DIR}/*.h")
  file(COPY ${SYPHON_HEADERS} DESTINATION "${CMAKE_BINARY_DIR}/syphon-include/Syphon")
  file(GLOB SYPHON_SOURCES "${syphon_SOURCE_DIR}/*.m" "${syphon_SOURCE_DIR}/*.c")
  # OpenGL version only: the Metal server would require compiling .metal shaders
  list(FILTER SYPHON_SOURCES EXCLUDE REGEX "Metal")
  add_library(syphon STATIC ${SYPHON_SOURCES})
  target_include_directories(syphon PUBLIC "${CMAKE_BINARY_DIR}/syphon-include" PRIVATE "${syphon_SOURCE_DIR}")
  target_compile_definitions(syphon PRIVATE SYPHON_CORE_SHARE GL_SILENCE_DEPRECATION)
  target_compile_options(syphon PRIVATE -w "SHELL:-include ${syphon_SOURCE_DIR}/Syphon_Prefix.pch"
    $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
  target_link_libraries(syphon PUBLIC "-framework Cocoa" "-framework OpenGL" "-framework IOSurface" "-framework Foundation")
  target_sources(fulskrin_engine PRIVATE src/engine/PublishSyphon.mm)
  set_source_files_properties(src/engine/PublishSyphon.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(fulskrin_engine PRIVATE syphon)
  target_compile_definitions(fulskrin_engine PRIVATE FULSKRIN_HAS_SYPHON)
endif()

if(WIN32 AND FULSKRIN_SPOUT)
  FetchContent_Declare(spout2
    GIT_REPOSITORY https://github.com/leadedge/Spout2.git
    GIT_TAG c2bcc12147711d12ace7d5f08e869d774d840f8a
    SOURCE_SUBDIR no-cmake) # download only: we build the sources ourselves
  FetchContent_MakeAvailable(spout2)
  set(SPOUT_DIR "${spout2_SOURCE_DIR}/SPOUTSDK/SpoutGL")
  add_library(spout_gl STATIC
    ${SPOUT_DIR}/Spout.cpp ${SPOUT_DIR}/SpoutCopy.cpp ${SPOUT_DIR}/SpoutDirectX.cpp
    ${SPOUT_DIR}/SpoutFrameCount.cpp ${SPOUT_DIR}/SpoutGL.cpp ${SPOUT_DIR}/SpoutGLextensions.cpp
    ${SPOUT_DIR}/SpoutReceiver.cpp ${SPOUT_DIR}/SpoutSender.cpp ${SPOUT_DIR}/SpoutSenderNames.cpp
    ${SPOUT_DIR}/SpoutSharedMemory.cpp ${SPOUT_DIR}/SpoutUtils.cpp)
  target_include_directories(spout_gl PUBLIC ${SPOUT_DIR})
  target_compile_definitions(spout_gl PUBLIC SPOUT_BUILD_STATIC)
  if(MSVC)
    target_compile_options(spout_gl PRIVATE /W0)
  endif()
  target_link_libraries(spout_gl PUBLIC opengl32 d3d9 d3d11 dxgi version winmm shlwapi)
  target_sources(fulskrin_engine PRIVATE src/engine/PublishSpout.cpp)
  target_link_libraries(fulskrin_engine PRIVATE spout_gl)
  target_compile_definitions(fulskrin_engine PRIVATE FULSKRIN_HAS_SPOUT)
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
set(APP_SOURCES
  src/main.cpp
  src/ui/MainWindow.cpp
  src/ui/LayerInspector.cpp
  src/ui/ParamPanel.cpp
  src/ui/MappingView.cpp
  src/ui/OutputWindow.cpp
  src/ui/GlDraw.cpp
  src/ui/Commands.cpp
  src/ui/LayerTable.cpp
  src/ui/MediaBin.cpp
  src/ui/MasterPanel.cpp
  src/ui/SettingsPanel.cpp
  src/ui/MemoryPanel.cpp
  src/ui/Widgets.cpp
)

if(APPLE)
  enable_language(OBJCXX)
  list(APPEND APP_SOURCES src/ui/MacPresentation.mm)
  add_executable(Fulskrin MACOSX_BUNDLE ${APP_SOURCES})
  target_link_libraries(Fulskrin PRIVATE "-framework AppKit")
  set_target_properties(Fulskrin PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Fulskrin"
    MACOSX_BUNDLE_GUI_IDENTIFIER "fr.fulskrin.app"
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
  set(ISF_DEST "$<TARGET_FILE_DIR:Fulskrin>/../Resources/isf")
elseif(WIN32)
  add_executable(Fulskrin WIN32 ${APP_SOURCES})
  set(ISF_DEST "$<TARGET_FILE_DIR:Fulskrin>/isf")
else()
  add_executable(Fulskrin ${APP_SOURCES})
  set(ISF_DEST "$<TARGET_FILE_DIR:Fulskrin>/isf")
endif()

target_include_directories(Fulskrin PRIVATE src src/ui)
target_link_libraries(Fulskrin PRIVATE fulskrin_engine Qt6::Widgets Qt6::OpenGLWidgets)

# Copy the bundled ISF shaders next to the executable (or into the macOS bundle).
# The folder is emptied first so renamed or deleted shaders do not linger in the build.
add_custom_command(TARGET Fulskrin POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${ISF_DEST}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/isf" "${ISF_DEST}")

# Windows: the FFmpeg DLLs must be next to Fulskrin.exe
if(WIN32 AND FFMPEG_ROOT)
  file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
  if(FFMPEG_DLLS)
    add_custom_command(TARGET Fulskrin POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} "$<TARGET_FILE_DIR:Fulskrin>")
  endif()
endif()

install(TARGETS Fulskrin BUNDLE DESTINATION . RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Engine tests (headless): cmake --build build --target fulskrin_tests
# ---------------------------------------------------------------------------
option(FULSKRIN_TESTS "Build the engine tests" ON)
if(FULSKRIN_TESTS)
  add_executable(fulskrin_tests EXCLUDE_FROM_ALL test/engine_test.cpp src/ui/Commands.cpp)
  target_include_directories(fulskrin_tests PRIVATE src/ui)
  target_link_libraries(fulskrin_tests PRIVATE fulskrin_engine)
  target_compile_definitions(fulskrin_tests PRIVATE TEST_DIR="${CMAKE_SOURCE_DIR}/test")
endif()
