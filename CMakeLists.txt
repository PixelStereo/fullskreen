cmake_minimum_required(VERSION 3.21)
project(Lanterne VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(MSVC)
  add_compile_options(/utf-8 /W3)   # sources en UTF-8 (textes de l'interface en français)
  add_compile_definitions(NOMINMAX _USE_MATH_DEFINES)
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

# ---------------------------------------------------------------------------
# FFmpeg : pkg-config (macOS Homebrew, Linux) ou FFMPEG_ROOT (Windows, build "shared")
# ---------------------------------------------------------------------------
set(FFMPEG_ROOT "" CACHE PATH "Dossier FFmpeg contenant include/ et lib/ (Windows)")
set(FFMPEG_LIBS avformat avcodec swscale avutil)

if(FFMPEG_ROOT)
  add_library(ffmpeg INTERFACE)
  target_include_directories(ffmpeg INTERFACE "${FFMPEG_ROOT}/include")
  foreach(lib ${FFMPEG_LIBS})
    find_library(FF_${lib} NAMES ${lib} lib${lib} PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(ffmpeg INTERFACE ${FF_${lib}})
  endforeach()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libavutil)
  add_library(ffmpeg INTERFACE)
  target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

# ---------------------------------------------------------------------------
# Moteur : aucune dépendance aux widgets (réutilisable avec une autre interface)
# ---------------------------------------------------------------------------
add_library(lanterne_engine STATIC
  src/engine/Gl.cpp
  src/engine/Isf.cpp
  src/engine/IsfLibrary.cpp
  src/engine/Mapping.cpp
  src/engine/VideoDecoder.cpp
  src/engine/Engine.cpp
  src/engine/Publish.cpp
)
target_include_directories(lanterne_engine PUBLIC src/engine)
target_link_libraries(lanterne_engine PUBLIC Qt6::Core Qt6::Gui Qt6::OpenGL ffmpeg)
target_compile_definitions(lanterne_engine PUBLIC GL_SILENCE_DEPRECATION)

# ---------------------------------------------------------------------------
# Publication de la sortie
#  - NDI et OMT : chargés à l'exécution (aucune dépendance de compilation)
#  - Syphon (macOS) et Spout (Windows) : sources téléchargées et compilées avec Lanterne
# ---------------------------------------------------------------------------
include(FetchContent)
option(LANTERNE_SYPHON "Publication Syphon (macOS)" ON)
option(LANTERNE_SPOUT "Publication Spout (Windows)" ON)

if(APPLE AND LANTERNE_SYPHON)
  enable_language(C OBJC OBJCXX) # Syphon : sources C et Objective-C
  FetchContent_Declare(syphon
    GIT_REPOSITORY https://github.com/Syphon/Syphon-Framework.git
    GIT_TAG f4761677a45b8034a3c2069ec0f3d2553da81fba
    SOURCE_SUBDIR pas-de-cmake) # téléchargement seul : on compile les sources nous-mêmes
  FetchContent_MakeAvailable(syphon)
  # Les sources importent <Syphon/...> : on expose les en-têtes sous ce préfixe.
  file(GLOB SYPHON_HEADERS "${syphon_SOURCE_DIR}/*.h")
  file(COPY ${SYPHON_HEADERS} DESTINATION "${CMAKE_BINARY_DIR}/syphon-include/Syphon")
  file(GLOB SYPHON_SOURCES "${syphon_SOURCE_DIR}/*.m" "${syphon_SOURCE_DIR}/*.c")
  # Version OpenGL uniquement : le serveur Metal demanderait la compilation de shaders .metal
  list(FILTER SYPHON_SOURCES EXCLUDE REGEX "Metal")
  add_library(syphon STATIC ${SYPHON_SOURCES})
  target_include_directories(syphon PUBLIC "${CMAKE_BINARY_DIR}/syphon-include" PRIVATE "${syphon_SOURCE_DIR}")
  target_compile_definitions(syphon PRIVATE SYPHON_CORE_SHARE GL_SILENCE_DEPRECATION)
  target_compile_options(syphon PRIVATE -w "SHELL:-include ${syphon_SOURCE_DIR}/Syphon_Prefix.pch"
    $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
  target_link_libraries(syphon PUBLIC "-framework Cocoa" "-framework OpenGL" "-framework IOSurface" "-framework Foundation")
  target_sources(lanterne_engine PRIVATE src/engine/PublishSyphon.mm)
  set_source_files_properties(src/engine/PublishSyphon.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(lanterne_engine PRIVATE syphon)
  target_compile_definitions(lanterne_engine PRIVATE LANTERNE_HAS_SYPHON)
endif()

if(WIN32 AND LANTERNE_SPOUT)
  FetchContent_Declare(spout2
    GIT_REPOSITORY https://github.com/leadedge/Spout2.git
    GIT_TAG c2bcc12147711d12ace7d5f08e869d774d840f8a
    SOURCE_SUBDIR pas-de-cmake) # téléchargement seul : on compile les sources nous-mêmes
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
  target_sources(lanterne_engine PRIVATE src/engine/PublishSpout.cpp)
  target_link_libraries(lanterne_engine PRIVATE spout_gl)
  target_compile_definitions(lanterne_engine PRIVATE LANTERNE_HAS_SPOUT)
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
)

if(APPLE)
  add_executable(Lanterne MACOSX_BUNDLE ${APP_SOURCES})
  set_target_properties(Lanterne PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Lanterne"
    MACOSX_BUNDLE_GUI_IDENTIFIER "fr.lanterne.app"
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
  set(ISF_DEST "$<TARGET_FILE_DIR:Lanterne>/../Resources/isf")
elseif(WIN32)
  add_executable(Lanterne WIN32 ${APP_SOURCES})
  set(ISF_DEST "$<TARGET_FILE_DIR:Lanterne>/isf")
else()
  add_executable(Lanterne ${APP_SOURCES})
  set(ISF_DEST "$<TARGET_FILE_DIR:Lanterne>/isf")
endif()

target_include_directories(Lanterne PRIVATE src src/ui)
target_link_libraries(Lanterne PRIVATE lanterne_engine Qt6::Widgets Qt6::OpenGLWidgets)

# Copie des shaders ISF fournis à côté de l'exécutable (ou dans le bundle macOS)
add_custom_command(TARGET Lanterne POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/isf" "${ISF_DEST}")

# Windows : les DLL FFmpeg doivent être à côté de Lanterne.exe
if(WIN32 AND FFMPEG_ROOT)
  file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
  if(FFMPEG_DLLS)
    add_custom_command(TARGET Lanterne POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} "$<TARGET_FILE_DIR:Lanterne>")
  endif()
endif()

install(TARGETS Lanterne BUNDLE DESTINATION . RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Tests du moteur (sans interface) : cmake --build build --target lanterne_tests
# ---------------------------------------------------------------------------
option(LANTERNE_TESTS "Compiler les tests du moteur" ON)
if(LANTERNE_TESTS)
  add_executable(lanterne_tests EXCLUDE_FROM_ALL test/engine_test.cpp src/ui/Commands.cpp)
  target_include_directories(lanterne_tests PRIVATE src/ui)
  target_link_libraries(lanterne_tests PRIVATE lanterne_engine)
  target_compile_definitions(lanterne_tests PRIVATE TEST_DIR="${CMAKE_SOURCE_DIR}/test")
endif()
