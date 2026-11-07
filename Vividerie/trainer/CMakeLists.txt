cmake_minimum_required(VERSION 3.25)
project(vividerie_mod LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/settings.cpp
    src/features/frame_pacing.cpp
    src/features/autoaim.cpp
)
target_compile_definitions(${PROJECT_NAME} PRIVATE "ImTextureID=ImU64")
target_link_libraries(${PROJECT_NAME} PRIVATE game)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".addon32")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".addon64")
endif()

add_library(game STATIC src/game/image.cpp src/game/runtime.cpp)
target_include_directories(game PUBLIC src)
target_compile_definitions(game PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(resolve_test tests/resolve_test.cpp)
target_link_libraries(resolve_test PRIVATE game)

include(FetchContent)

# safetyhook
set(SAFETYHOOK_FETCH_ZYDIS ON)
FetchContent_Declare(
    safetyhook
    GIT_REPOSITORY "https://github.com/cursey/safetyhook.git"
    GIT_TAG "origin/main"
)
FetchContent_MakeAvailable(safetyhook)
target_link_libraries(${PROJECT_NAME} PRIVATE safetyhook)
target_link_libraries(game PUBLIC safetyhook)

# imgui
add_library(imgui INTERFACE)
target_include_directories(imgui INTERFACE external/reshade/deps/imgui)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui)

# reshade
add_library(reshade INTERFACE)
target_include_directories(reshade INTERFACE external/reshade/include)
target_link_libraries(${PROJECT_NAME} PRIVATE reshade)
