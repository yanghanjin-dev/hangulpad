# Resolve transitive DLLs too (libzip depends on compression/crypto libraries).
cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${PROGRAM}"
    DIRECTORIES "${TOOLCHAIN_BIN}"
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR missing
    PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
    POST_EXCLUDE_REGEXES [[.*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\/][Ss][Yy][Ss][Tt][Ee][Mm]32[\\/].*]])
if(missing)
    message(FATAL_ERROR "Unresolved runtime DLLs: ${missing}")
endif()
get_filename_component(destination "${PROGRAM}" DIRECTORY)
foreach(dependency IN LISTS dependencies)
    get_filename_component(source_dir "${dependency}" DIRECTORY)
    if(NOT source_dir STREQUAL destination)
        file(COPY "${dependency}" DESTINATION "${destination}")
    endif()
endforeach()
