cmake_minimum_required(VERSION 3.21)

project(
    CameraCull
    VERSION 1.0.0
    LANGUAGES CXX
)

# ── C++ standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)   # LTO in Release

# ── vcpkg / CommonLibSSE-NG ───────────────────────────────────────────────────
# CommonLibSSE-NG is consumed via vcpkg; see vcpkg.json.
find_package(CommonLibSSE CONFIG REQUIRED)
find_package(simpleini CONFIG REQUIRED)       # header-only INI parser

# ── Plugin target ─────────────────────────────────────────────────────────────
add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES
        src/main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    src
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    simpleini::simpleini
)

# MSVC: warnings + disable RTTI (CommonLibSSE handles it internally)
target_compile_options(${PROJECT_NAME} PRIVATE
    /W4
    /WX            # warnings as errors
    /permissive-
    /Zc:preprocessor
    $<$<CONFIG:Debug>:/Od /Zi>
    $<$<CONFIG:Release>:/O2 /DNDEBUG>
)

# ── Install: copy .dll + default INI into a staging folder ────────────────────
# GitHub Actions will zip this folder and attach it as a release artifact.
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION "SKSE/Plugins"
)
install(FILES
    "${CMAKE_SOURCE_DIR}/CameraCull.ini"
    DESTINATION "SKSE/Plugins"
)
