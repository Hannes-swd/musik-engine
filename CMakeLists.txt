cmake_minimum_required(VERSION 3.20)
project(MusicEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# --- miniaudio (Audio-Ausgabe) ---
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.21
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)
endif()

# --- GLFW (Fenster) ---
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw)

# --- Dear ImGui (UI) ---
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b-docking    # Docking-Version: Fenster andocken / als Tabs anordnen
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(imgui PUBLIC glfw opengl32)

# --- nlohmann/json (Song-Dateien) ---
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

# --- Engine-Kern (ohne UI) ---
add_library(musiccore STATIC
    src/audio/AudioEngine.cpp
    src/audio/Sequencer.cpp
    src/audio/miniaudio_impl.c
    src/instruments/PianoSynth.cpp
    src/instruments/InstrumentRegistry.cpp
    src/instruments/Synths.cpp
    src/theory/Theory.cpp
    src/generator/Generator.cpp
    src/io/SongIO.cpp
    src/analysis/Analyzer.cpp
)
target_include_directories(musiccore PUBLIC src ${miniaudio_SOURCE_DIR})
target_link_libraries(musiccore PUBLIC nlohmann_json::nlohmann_json)

# --- App ---
add_executable(MusicEngine
    src/main.cpp
    src/ui/PianoRoll.cpp
    src/ui/GeneratorPanel.cpp
    src/ui/FileDialog.cpp
    src/ui/LivePlayer.cpp
    src/ui/ScoreView.cpp
    src/ui/ArrangeView.cpp
    src/ui/MixerView.cpp
    src/ui/ChordSheetView.cpp
    src/ui/DrumGridView.cpp
    src/ui/VisualizerView.cpp
    src/ui/PlayView.cpp
    src/io/MidiInput.cpp
)
target_link_libraries(MusicEngine PRIVATE musiccore imgui)
if(WIN32)
    target_link_libraries(MusicEngine PRIVATE comdlg32 winmm)
endif()

# Gleiches Programm als zweite .exe für Tests/Analysen (lässt sich bauen, während MusicEngine.exe offen ist)
get_target_property(ME_SOURCES MusicEngine SOURCES)
add_executable(MusicEngineCli ${ME_SOURCES})
target_link_libraries(MusicEngineCli PRIVATE musiccore imgui)
if(WIN32)
    target_link_libraries(MusicEngineCli PRIVATE comdlg32 winmm)
endif()
if(MSVC)
    target_compile_options(MusicEngineCli PRIVATE /W4 /utf-8)
endif()

# --- Notenschrift Bravura (SMuFL, SIL Open Font License) ---
set(BRAVURA_FONT ${CMAKE_BINARY_DIR}/Bravura.otf)
if(NOT EXISTS ${BRAVURA_FONT})
    file(DOWNLOAD https://github.com/steinbergmedia/bravura/raw/master/redist/otf/Bravura.otf ${BRAVURA_FONT}
         STATUS BRAVURA_STATUS)
    list(GET BRAVURA_STATUS 0 BRAVURA_CODE)
    if(NOT BRAVURA_CODE EQUAL 0)
        message(WARNING "Bravura-Font konnte nicht geladen werden - Notenansicht nutzt einfache Ersatzgrafik.")
        file(REMOVE ${BRAVURA_FONT})
    endif()
endif()
if(EXISTS ${BRAVURA_FONT})
    add_custom_command(TARGET MusicEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BRAVURA_FONT} $<TARGET_FILE_DIR:MusicEngine>/Bravura.otf)
endif()

if(MSVC)
    target_compile_options(musiccore PRIVATE /W4 /utf-8)
    target_compile_options(MusicEngine PRIVATE /W4 /utf-8)
endif()
