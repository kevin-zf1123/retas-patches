cmake_minimum_required(VERSION 3.19)
project(RetasPlugins LANGUAGES NONE)
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(STYLOS_HOST_DIR "" CACHE PATH "Reference images named <variant>/<stage>.exe")
set(RETAS_ZIG "zig" CACHE STRING "Zig 0.13 executable")
set(RETAS_TCC "tcc" CACHE STRING "x86 Tiny C Compiler")
set(destination "${CMAKE_CURRENT_SOURCE_DIR}/packages/0.2.1")
set(extra)
if(STYLOS_HOST_DIR)
    list(APPEND extra --refs "${STYLOS_HOST_DIR}")
endif()

# This only discovers targets to build in this checkout. The manager binary
# never consumes this list and can install a package built outside CMake.
file(GLOB_RECURSE recipes CONFIGURE_DEPENDS "better-canvas/build.json" "stylos/build.json")
add_custom_target(components)
foreach(recipe IN LISTS recipes)
    get_filename_component(folder "${recipe}" DIRECTORY)
    file(READ "${folder}/plugin.json" manifest)
    string(JSON id GET "${manifest}" id)
    add_custom_target("${id}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_plugins.py"
                --only "${id}" --out "${destination}/plugins" --zig "${RETAS_ZIG}" --tcc "${RETAS_TCC}" ${extra}
        VERBATIM)
    add_dependencies(components "${id}")
endforeach()

add_custom_target(plugin-manager
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/plugin-manager/build.py"
            --base "${STYLOS_HOST_DIR}/chs/base.exe" --base "${STYLOS_HOST_DIR}/chs-laa/base.exe"
            --zig "${RETAS_ZIG}" --out "${destination}"
    VERBATIM)
add_custom_target(devkit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/retas-devkit/build.py" --out "${destination}/devkit" VERBATIM)
if(STYLOS_HOST_DIR)
    add_custom_target(packages ALL)
else()
    add_custom_target(packages)
endif()
add_dependencies(packages plugin-manager components devkit)
