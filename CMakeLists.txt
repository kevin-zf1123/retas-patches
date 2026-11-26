cmake_minimum_required(VERSION 3.19)
project(RetasPackages LANGUAGES NONE)
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

set(manager "${CMAKE_CURRENT_SOURCE_DIR}/plugin-manager/stylos")
set(staging "${CMAKE_CURRENT_BINARY_DIR}/components")
set(destination "${CMAKE_CURRENT_SOURCE_DIR}/packages")
set(registry_file "${manager}/registry.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${registry_file}")
file(READ "${registry_file}" registry)
string(JSON count LENGTH "${registry}" plugins)
math(EXPR last "${count} - 1")

# Each component owns one archive. Keep native DLLs in separate directories
# because both tablet providers export a file named wintab32.dll.
foreach(i RANGE 0 ${last})
    foreach(field IN ITEMS id package manifest)
        string(JSON ${field} GET "${registry}" plugins ${i} ${field})
    endforeach()
    get_filename_component(src "${CMAKE_CURRENT_SOURCE_DIR}/${manifest}" DIRECTORY)
    set(runtime_args)
    set(runtime_files)
    if(id MATCHES "^(motion-check|tablet-native|windows-ink)$")
        if(NOT CMAKE_HOST_WIN32)
            continue()
        endif()
        set(runtime_dir "${CMAKE_CURRENT_BINARY_DIR}/runtime/${id}")
        set(runtime_sources "${src}/Build.ps1")
        set(worker_output)
        if(id STREQUAL "motion-check")
            set(runtime_files "${runtime_dir}/MotionRender.dll")
            set(worker_output "${runtime_dir}/StylosRender64.exe")
            file(GLOB_RECURSE worker_sources CONFIGURE_DEPENDS "${src}/worker/*")
            add_custom_command(OUTPUT "${worker_output}"
                COMMAND "${Python3_EXECUTABLE}" "${src}/worker/build.py"
                        --build "${CMAKE_CURRENT_BINARY_DIR}/render64" --out "${worker_output}"
                DEPENDS ${worker_sources} "${src}/src/frame_cache.h" "${src}/src/render_settings.h" "${src}/src/cache_space.h" VERBATIM)
            list(APPEND runtime_sources "${src}/src/engine.c" "${src}/src/engine.h"
                 "${src}/src/scheduler.inc" "${src}/src/frame_cache.h" "${src}/src/render_settings.h" "${src}/src/cache_space.h" "${src}/versions.json")
        elseif(id STREQUAL "tablet-native")
            set(runtime_files "${runtime_dir}/wintab32.dll")
            list(APPEND runtime_sources "${src}/NativeWinTabCoordinates.c" "${src}/Stylos-Wacom.cmd")
            file(GLOB_RECURSE canvas_sources CONFIGURE_DEPENDS
                "better-canvas/*.c" "better-canvas/*.h" "better-canvas/*.S")
            list(APPEND runtime_sources ${canvas_sources})
        else()
            set(runtime_files "${runtime_dir}/StylosInk.dll" "${runtime_dir}/wintab32.dll")
            list(APPEND runtime_sources "${src}/StylosInk.cs" "${src}/WinTabExports.il")
        endif()
        add_custom_command(OUTPUT ${runtime_files}
            COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${src}/Build.ps1"
                    -OutputDirectory "${runtime_dir}"
            DEPENDS ${runtime_sources}
            VERBATIM)
        list(APPEND runtime_files ${worker_output})
        set(runtime_args --runtime-dir "${runtime_dir}")
    endif()
    set(guide "${src}/README.md")
    if(id STREQUAL "inbetween")
        set(guide "${manager}/MIDPOINT.md")
    endif()
    set(archive "${staging}/${package}")
    add_custom_command(OUTPUT "${archive}"
        COMMAND "${Python3_EXECUTABLE}" "${manager}/component_package.py" "${id}"
                --out "${staging}" ${runtime_args}
        DEPENDS "${manager}/component_package.py" "${registry_file}" "${manifest}"
                "${guide}" LICENSE ${runtime_files}
        VERBATIM)
    add_custom_target(${id}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${destination}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${archive}" "${destination}/${package}"
        DEPENDS "${archive}"
        VERBATIM)
    list(APPEND component_targets "${id}")
endforeach()

add_custom_target(components ALL)
add_dependencies(components ${component_targets})

if(CMAKE_HOST_WIN32)
    set(STYLOS_HOST_DIR "" CACHE PATH "Reference EXEs named <variant>/<stage>.exe")
    set(STYLOS_INBETWEEN_EXE "" CACHE FILEPATH "Official v0.1 StylosInbetween.exe")
    if(STYLOS_HOST_DIR AND STYLOS_INBETWEEN_EXE)
        set(versions_file "${CMAKE_CURRENT_SOURCE_DIR}/stylos/motion-check/versions.json")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${versions_file}")
        file(READ "${versions_file}" versions)
        string(JSON count LENGTH "${versions}" variants)
        math(EXPR last "${count} - 1")
        foreach(i RANGE 0 ${last})
            string(JSON variant GET "${versions}" variants ${i} id)
            foreach(stage IN ITEMS base stage1 main)
                set(host "${STYLOS_HOST_DIR}/${variant}/${stage}.exe")
                if(NOT EXISTS "${host}")
                    message(FATAL_ERROR "Missing Stylos reference image: ${host}")
                endif()
                string(JSON expected GET "${versions}" variants ${i} ${stage})
                string(TOLOWER "${expected}" expected)
                file(SHA256 "${host}" actual)
                if(NOT actual STREQUAL expected)
                    message(FATAL_ERROR "Incorrect ${variant}/${stage}.exe: expected SHA-256 ${expected}")
                endif()
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${host}")
                list(APPEND host_args --host "${host}")
            endforeach()
        endforeach()
        if(NOT EXISTS "${STYLOS_INBETWEEN_EXE}")
            message(FATAL_ERROR "Missing v0.1 installer: ${STYLOS_INBETWEEN_EXE}")
        endif()
        set(linked "${CMAKE_CURRENT_BINARY_DIR}/manager")
        add_custom_target(plugin-manager
            COMMAND "${Python3_EXECUTABLE}" "${manager}/build_host_packages.py"
                    ${host_args} --inbetween "${STYLOS_INBETWEEN_EXE}"
                    --components "${staging}" --out "${linked}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${linked}/Stylos-Plugin-Manager-x86.zip" "${destination}/Stylos-Plugin-Manager-x86.zip"
            VERBATIM)
        add_dependencies(plugin-manager components)
        add_custom_target(packages ALL)
    else()
        add_custom_target(plugin-manager
            COMMAND "${CMAKE_COMMAND}" -E echo "Configure STYLOS_HOST_DIR and STYLOS_INBETWEEN_EXE to build the manager."
            COMMAND "${CMAKE_COMMAND}" -E false VERBATIM)
        add_custom_target(packages)
    endif()
    add_dependencies(packages plugin-manager)
endif()
