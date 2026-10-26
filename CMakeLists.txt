cmake_minimum_required(VERSION 3.22)
project(AxellGate VERSION 4.0.0)
# the version for the GUI (every target: the plugin, tests and tools draw the same header)
add_compile_definitions(AXELL_VERSION_STRING="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG        8.0.15
  GIT_SHALLOW    ON)
FetchContent_MakeAvailable(JUCE)

# Axell core (shared DSP + look): next to this project, or vendored into external/axell-core for releases
set(AXELL_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/external/axell-core" CACHE PATH "Axell core")
if(NOT EXISTS "${AXELL_CORE_DIR}/CMakeLists.txt")
  set(AXELL_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../axell-core" CACHE PATH "Axell core" FORCE)
endif()
add_subdirectory(${AXELL_CORE_DIR} axell-core)

juce_add_plugin(AxellGate
  COMPANY_NAME              "Axell Yobag"
  COMPANY_EMAIL             "axellpetre@protonmail.com"
  BUNDLE_ID                 com.axellyobag.axellgate
  PLUGIN_MANUFACTURER_CODE  Axyb
  PLUGIN_CODE               Gte1
  FORMATS                   VST3
  PRODUCT_NAME              "Axell Gate"
  IS_SYNTH                  FALSE
  NEEDS_MIDI_INPUT          FALSE
  NEEDS_MIDI_OUTPUT         FALSE
  IS_MIDI_EFFECT            FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD   FALSE
  VST3_CATEGORIES           Fx Dynamics)

set(GATE_SOURCES Source/PluginProcessor.cpp Source/PluginEditor.cpp)
set(GATE_DEFS JUCE_WEB_BROWSER=0 AXELL_THEME_HUE=255 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_sources(AxellGate PRIVATE ${GATE_SOURCES})
target_compile_definitions(AxellGate PUBLIC ${GATE_DEFS})
axell_core_add_gui(AxellGate)
target_link_libraries(AxellGate PRIVATE juce::juce_audio_utils
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

option(GATE_SANITIZE "AddressSanitizer + UBSan for the test programs" OFF)
function(gate_sanitize target)
  if(GATE_SANITIZE)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

# DSP suite (JUCE-free: the engine headers exactly as they ship)
enable_testing()
add_executable(GateDspTests tests/DspTests.cpp)
target_include_directories(GateDspTests PRIVATE Source)
target_link_libraries(GateDspTests PRIVATE axell_core_dsp)
target_compile_options(GateDspTests PRIVATE -Wall -Wextra -Wshadow -Wno-mismatched-new-delete $<$<CONFIG:Release>:-O3>)
gate_sanitize(GateDspTests)
add_test(NAME dsp_suite COMMAND GateDspTests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# plugin suite + GUI snapshots (JUCE)
foreach(tool GatePluginTests GateGuiSnapshot)
  juce_add_console_app(${tool} PRODUCT_NAME "${tool}")
  target_compile_definitions(${tool} PRIVATE ${GATE_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1 JucePlugin_Name="Axell Gate")
  target_link_libraries(${tool} PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)
  axell_core_add_gui(${tool})
  gate_sanitize(${tool})
endforeach()
target_sources(GatePluginTests PRIVATE tests/PluginTests.cpp ${GATE_SOURCES})
target_sources(GateGuiSnapshot PRIVATE tests/GuiSnapshot.cpp ${GATE_SOURCES})
add_test(NAME plugin_suite COMMAND GatePluginTests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# independent black-box audit host (from the core)
juce_add_console_app(GateAuditHost PRODUCT_NAME "GateAuditHost")
target_sources(GateAuditHost PRIVATE ${AXELL_CORE_DIR}/tools/AuditHost.cpp)
target_compile_definitions(GateAuditHost PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(GateAuditHost PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_recommended_config_flags)

# research / benchmark tools (JUCE-free; the shipping engine headers)
foreach(tool GateFeatures GateRender)
  add_executable(${tool} tools/${tool}.cpp)
  target_include_directories(${tool} PRIVATE Source)
  target_link_libraries(${tool} PRIVATE axell_core_dsp)
  target_compile_options(${tool} PRIVATE -O3)
endforeach()
