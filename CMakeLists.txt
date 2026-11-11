cmake_minimum_required(VERSION 3.21)
project(WorldForge VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
option(WORLDFORGE_BUILD_EDITOR "Build the Qt desktop editor" ON)
option(WORLDFORGE_BUILD_EDITOR_AUDIT "Build the isolated editor UI audit" OFF)

function(worldforge_compile_options target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(WorldForgeCore STATIC
    Engine/Core/Guid.cpp Engine/Core/Guid.h
    Engine/Scene/SceneDocument.cpp Engine/Scene/SceneDocument.h
    Engine/Geometry/Mesh.cpp Engine/Geometry/Mesh.h
)
target_include_directories(WorldForgeCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
worldforge_compile_options(WorldForgeCore)

if(BUILD_TESTING)
    add_executable(WorldForgeCoreTests Tests/SceneDocumentTests.cpp)
    target_link_libraries(WorldForgeCoreTests PRIVATE WorldForgeCore)
    worldforge_compile_options(WorldForgeCoreTests)
    add_test(NAME WorldForge.Core COMMAND WorldForgeCoreTests)
endif()

if(NOT WORLDFORGE_BUILD_EDITOR)
    return()
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg)
qt_standard_project_setup()

set(editor_sources
    App/MainWindow.h App/MainWindow.cpp
    Components/EditorPanels.h Components/EditorPanels.cpp
    Components/Viewport.h Components/Viewport.cpp
    Components/LiveViewport.h Components/LiveViewport.cpp
    DesignSystem/Icons.h DesignSystem/Icons.cpp
    State/ProjectStore.h State/ProjectStore.cpp
    State/GlbImport.h State/GlbImport.cpp
    Workspaces/WorkspaceCatalog.h Workspaces/WorkspaceCatalog.cpp
    Workspaces/StudioWorkspace.h Workspaces/StudioWorkspace.cpp
    Workspaces/StudioViews.h Workspaces/StudioViews.cpp
)
set(concept_art
    Docs/file_000000000a4481f5b152fe135f0875fb.png
    Docs/file_000000001b8c8211a2910938bbbedb44.png
    Docs/file_000000002aac82099c4b6a6130892725.png
    Docs/file_00000000389081f9930a2924e08d6fe0.png
    Docs/file_000000003e18820d85b6a524ad596f27.png
    Docs/file_00000000418c8208a492dcb2b14aaa90.png
    Docs/file_00000000437082119cebcfae73fcd61e.png
    Docs/file_000000004db482119a8a3c703124c125.png
    Docs/file_00000000569c82128e96b4d620ab1fa7.png
    Docs/file_0000000064b4820b8061f40d48c49509.png
    Docs/file_00000000694c820ead12cda391661601.png
    Docs/file_000000007e9c821183f934c945a7f4da.png
    Docs/file_000000009a5c8211a4f785ae1dbbb4d3.png
    Docs/file_00000000cae881f583b6c2525193d90e.png
    Docs/file_00000000ffdc81faa622de2e167904bf.png
)

find_program(WORLDFORGE_GLSLANG NAMES glslang glslangValidator
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{LOCALAPPDATA}/WorldForge/Vulkan/glslang-16.6.0/bin")
if(NOT Vulkan_INCLUDE_DIR AND EXISTS "$ENV{LOCALAPPDATA}/WorldForge/Vulkan/Vulkan-Headers/include/vulkan/vulkan.h")
    set(Vulkan_INCLUDE_DIR "$ENV{LOCALAPPDATA}/WorldForge/Vulkan/Vulkan-Headers/include")
endif()
if(Vulkan_INCLUDE_DIR AND WORLDFORGE_GLSLANG)
    foreach(shader Scene.vert Scene.frag)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.spv")
        add_custom_command(OUTPUT "${output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
            COMMAND "${WORLDFORGE_GLSLANG}" -V "${CMAKE_CURRENT_SOURCE_DIR}/Components/Shaders/${shader}" -o "${output}"
            DEPENDS "Components/Shaders/${shader}" VERBATIM)
        list(APPEND scene_shaders "${output}")
    endforeach()
    add_custom_target(WorldForgeSceneShaders DEPENDS ${scene_shaders})
else()
    message(STATUS "Live Vulkan viewport disabled. Install the Vulkan headers and glslang.")
endif()

function(configure_editor target)
    worldforge_compile_options(${target})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE WorldForgeCore Qt6::Widgets Qt6::Svg)

    qt_add_resources(${target} "${target}_theme"
        PREFIX "/"
        FILES DesignSystem/WorldForgeTheme.qss DesignSystem/chevron.svg DesignSystem/check.svg Data/demo.json Data/studios.json
    )
    qt_add_resources(${target} "${target}_concept_art"
        BIG_RESOURCES PREFIX "/" OPTIONS --no-compress FILES ${concept_art}
    )
    if(TARGET WorldForgeSceneShaders)
        add_dependencies(${target} WorldForgeSceneShaders)
        target_compile_definitions(${target} PRIVATE WORLDFORGE_VULKAN)
        target_include_directories(${target} SYSTEM PRIVATE "${Vulkan_INCLUDE_DIR}")
        qt_add_resources(${target} "${target}_scene_shaders" PREFIX "/"
            BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${scene_shaders})
    endif()
endfunction()

qt_add_executable(WorldForgeEditor WIN32 MACOSX_BUNDLE App/main.cpp ${editor_sources})
configure_editor(WorldForgeEditor)

if(BUILD_TESTING AND WORLDFORGE_BUILD_EDITOR_AUDIT)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(WorldForgeEditorAudit Tests/EditorAudit.cpp ${editor_sources})
    configure_editor(WorldForgeEditorAudit)
    target_link_libraries(WorldForgeEditorAudit PRIVATE Qt6::Test)
    add_test(NAME WorldForge.EditorAudit
        COMMAND WorldForgeEditorAudit "${CMAKE_CURRENT_BINARY_DIR}/qa" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(WorldForge.EditorAudit PROPERTIES TIMEOUT 360)
endif()

install(TARGETS WorldForgeEditor RUNTIME DESTINATION bin BUNDLE DESTINATION .)
if(WIN32)
    qt_generate_deploy_app_script(
        TARGET WorldForgeEditor
        OUTPUT_SCRIPT deploy_script
        NO_TRANSLATIONS
    )
    install(SCRIPT "${deploy_script}")
endif()
