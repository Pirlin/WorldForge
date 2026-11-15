cmake_minimum_required(VERSION 3.21)
project(WorldForge VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
option(WORLDFORGE_BUILD_EDITOR "Build the Qt desktop editor" ON)
option(WORLDFORGE_BUILD_EDITOR_AUDIT "Build the isolated editor UI audit" OFF)

include(FetchContent)
set(OVERRIDE_CXX_FLAGS OFF)
set(INTERPROCEDURAL_OPTIMIZATION OFF)
set(ENABLE_ALL_WARNINGS OFF)
set(ENABLE_INSTALL OFF)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "Use the same runtime library as WorldForge")
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF)
set(ENABLE_OBJECT_STREAM OFF)
set(USE_SSE4_1 OFF)
set(USE_SSE4_2 OFF)
set(USE_AVX OFF)
set(USE_AVX2 OFF)
set(USE_LZCNT OFF)
set(USE_TZCNT OFF)
set(USE_F16C OFF)
set(USE_FMADD OFF)
FetchContent_Declare(Jolt
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG 0373ec0dd762e4bc2f6acdb08371ee84fa23c6db
    SOURCE_SUBDIR Build)
FetchContent_MakeAvailable(Jolt)
install(FILES "${jolt_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME Jolt-LICENSE.txt)

set(manifold_directory_option)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(manifold_directory_option EXCLUDE_FROM_ALL)
endif()
FetchContent_Declare(manifold
    GIT_REPOSITORY https://github.com/elalish/manifold.git
    GIT_TAG 798d83c8d7fabcddd23c1617097b95ba40f2597c
    ${manifold_directory_option})
function(worldforge_add_manifold)
    set(BUILD_SHARED_LIBS OFF)
    set(MANIFOLD_CROSS_SECTION OFF)
    set(MANIFOLD_DOWNLOADS OFF)
    set(MANIFOLD_TEST OFF)
    set(MANIFOLD_STRICT OFF)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
        FetchContent_MakeAvailable(manifold)
    else()
        FetchContent_Populate(manifold)
        add_subdirectory("${manifold_SOURCE_DIR}" "${manifold_BINARY_DIR}" EXCLUDE_FROM_ALL)
    endif()
    install(FILES "${manifold_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME Manifold-LICENSE.txt)
endfunction()
worldforge_add_manifold()

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
    Engine/Geometry/MeshBoolean.cpp Engine/Geometry/MeshBoolean.h
    Engine/Geometry/MeshRemesh.cpp Engine/Geometry/MeshRemesh.h
    Engine/Geometry/SculptBrush.cpp Engine/Geometry/SculptBrush.h
    Engine/Geometry/MeshTopology.cpp Engine/Geometry/MeshTopology.h
    Engine/Geometry/MeshModifiers.cpp Engine/Geometry/MeshModifiers.h
    Engine/Geometry/MeshUv.cpp Engine/Geometry/MeshUv.h
    Engine/World/WorldSimulation.cpp Engine/World/WorldSimulation.h
    Engine/World/PlayableScene.cpp Engine/World/PlayableScene.h
    Engine/World/Gameplay.cpp Engine/World/Gameplay.h
)
target_include_directories(WorldForgeCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(WorldForgeCore PRIVATE Jolt manifold)
worldforge_compile_options(WorldForgeCore)

if(WIN32)
    add_executable(WorldForgePlayer WIN32 Player/main.cpp)
    target_link_libraries(WorldForgePlayer PRIVATE WorldForgeCore d3d11 dxgi d3dcompiler shell32 comdlg32
        windowscodecs shlwapi ole32 psapi)
    target_compile_definitions(WorldForgePlayer PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    worldforge_compile_options(WorldForgePlayer)
    install(TARGETS WorldForgePlayer RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
    add_executable(WorldForgeCoreTests Tests/SceneDocumentTests.cpp)
    target_link_libraries(WorldForgeCoreTests PRIVATE WorldForgeCore)
    worldforge_compile_options(WorldForgeCoreTests)
    add_test(NAME WorldForge.Core COMMAND WorldForgeCoreTests)
    if(TARGET WorldForgePlayer)
        set(standalone_fixture "${CMAKE_CURRENT_BINARY_DIR}/standalone-smoke.wfplay")
        add_test(NAME WorldForge.StandaloneFixture COMMAND WorldForgeCoreTests "${standalone_fixture}")
        set_tests_properties(WorldForge.StandaloneFixture PROPERTIES FIXTURES_SETUP StandaloneFixture)
        add_test(NAME WorldForge.PlayerWithoutEditor COMMAND WorldForgePlayer --smoke-test-warp "${standalone_fixture}")
        set_tests_properties(WorldForge.PlayerWithoutEditor PROPERTIES FIXTURES_REQUIRED StandaloneFixture TIMEOUT 45)
    endif()
endif()

if(NOT WORLDFORGE_BUILD_EDITOR)
    return()
endif()

find_path(Vulkan_INCLUDE_DIR NAMES vulkan/vulkan.h
    HINTS "$ENV{VULKAN_SDK}/Include" "$ENV{LOCALAPPDATA}/WorldForge/Vulkan/Vulkan-Headers/include")
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg)
qt_standard_project_setup()

set(editor_sources
    Components/WorldLightingVulkan.cpp Components/WorldLightingVulkan.h
    App/EditorLog.cpp App/EditorLog.h
    Components/CommandTerminal.cpp Components/CommandTerminal.h
    Components/ProjectLauncher.cpp Components/ProjectLauncher.h
    Components/WindowCaption.cpp Components/WindowCaption.h
    App/MainWindow.h App/MainWindow.cpp
    App/SystemMetrics.h App/SystemMetrics.cpp
    Components/EditorPanels.h Components/EditorPanels.cpp
    Components/Viewport.h Components/Viewport.cpp
    Components/LiveViewport.h Components/LiveViewport.cpp
    DesignSystem/Icons.h DesignSystem/Icons.cpp
    State/ProjectStore.h State/ProjectStore.cpp
    State/GlbImport.h State/GlbImport.cpp
    State/StudioPublishReview.h State/StudioPublishReview.cpp
    State/ForgeAi.h State/ForgeAi.cpp
    State/PlayableStreet.h State/PlayableStreet.cpp
    Workspaces/ForgeAiJob.h Workspaces/ForgeAiJob.cpp
    Components/ForgeAiPanel.h Components/ForgeAiPanel.cpp
    State/WorldScene.h State/WorldScene.cpp
    State/WorldRuntime.h State/WorldRuntime.cpp
    Workspaces/WorkspaceCatalog.h Workspaces/WorkspaceCatalog.cpp
    Workspaces/StudioWorkspace.h Workspaces/StudioWorkspace.cpp
    Workspaces/StudioViews.h Workspaces/StudioViews.cpp
    Workspaces/EmbeddedStudio.h Workspaces/EmbeddedStudio.cpp
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
if(Vulkan_INCLUDE_DIR AND WORLDFORGE_GLSLANG)
    foreach(shader Scene.vert Scene.frag Sky.vert Sky.frag Shadow.vert SelectionOutline.vert SelectionOutline.frag)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.spv")
        add_custom_command(OUTPUT "${output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
            COMMAND "${WORLDFORGE_GLSLANG}" -V "${CMAKE_CURRENT_SOURCE_DIR}/Components/Shaders/${shader}" -o "${output}"
            DEPENDS "Components/Shaders/${shader}" "Components/Shaders/Lighting.glsl" VERBATIM)
        list(APPEND scene_shaders "${output}")
    endforeach()
    add_custom_target(WorldForgeSceneShaders DEPENDS ${scene_shaders})
else()
    message(STATUS "Live Vulkan viewport disabled. Install the Vulkan headers and glslang.")
endif()

function(configure_editor target)
    qt_add_resources(${target} "${target}_studio_publish" PREFIX "/"
        FILES Workspaces/StudioPublish.py Workspaces/StudioNavigation.py Workspaces/ForgeJob.py)
    worldforge_compile_options(${target})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE WorldForgeCore Qt6::Widgets Qt6::Svg)
    if(WIN32)
        target_link_libraries(${target} PRIVATE pdh dxgi)
    endif()

    qt_add_resources(${target} "${target}_theme"
        PREFIX "/"
        FILES DesignSystem/WorldForgeTheme.qss DesignSystem/chevron.svg DesignSystem/check.svg Data/demo.json Data/studios.json
    )
    qt_add_resources(${target} "${target}_concept_art"
        BIG_RESOURCES PREFIX "/" OPTIONS --no-compress FILES ${concept_art}
    )
    qt_add_resources(${target} "${target}_launcher_art"
        BIG_RESOURCES PREFIX "/" OPTIONS --no-compress
        FILES Data/Launcher/open-world-city.jpg Data/Launcher/fantasy-rpg.jpg Data/Launcher/action-adventure.jpg
              Data/Launcher/racing-experience.jpg Data/Launcher/shooter-arena.jpg
              Data/Launcher/survival-sandbox.jpg Data/Launcher/simulation-city.jpg Data/Launcher/skyline-banner.png
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
    qt_add_executable(WorldForgeEditorAudit Tests/EditorAudit.cpp ${editor_sources}
        Workspaces/ModelingWorkspace.h Workspaces/ModelingWorkspace.cpp
        Workspaces/SculptingWorkspace.h Workspaces/SculptingWorkspace.cpp
        Workspaces/SurfaceWorkspace.h Workspaces/SurfaceWorkspace.cpp)
    configure_editor(WorldForgeEditorAudit)
    target_link_libraries(WorldForgeEditorAudit PRIVATE Qt6::Test)
    add_test(NAME WorldForge.EditorAudit
        COMMAND WorldForgeEditorAudit "${CMAKE_CURRENT_BINARY_DIR}/qa" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(WorldForge.EditorAudit PROPERTIES TIMEOUT 360)
    if(TARGET WorldForgeSceneShaders)
        add_test(NAME WorldForge.Authoring
            COMMAND WorldForgeEditorAudit "${CMAKE_CURRENT_BINARY_DIR}/authoring-qa" "${CMAKE_CURRENT_SOURCE_DIR}" --authoring)
        set_tests_properties(WorldForge.Authoring PROPERTIES TIMEOUT 45)
        if(TARGET WorldForgePlayer)
            set_tests_properties(WorldForge.Authoring PROPERTIES FIXTURES_SETUP PlayableScene)
            add_test(NAME WorldForge.Player COMMAND WorldForgePlayer --smoke-test
                "${CMAKE_CURRENT_BINARY_DIR}/authoring-qa/outdoor.wfplay")
            set_tests_properties(WorldForge.Player PROPERTIES FIXTURES_REQUIRED PlayableScene TIMEOUT 30)
        endif()
    endif()
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
