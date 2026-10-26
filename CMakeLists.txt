cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
project(HangulPad LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
file(STRINGS "include/hp/version.h" version_line REGEX "^#define HP_VERSION_STRING")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" HP_VERSION "${version_line}")
find_package(libzip CONFIG REQUIRED)
add_library(hangulpad_engine STATIC
    src/common.c src/ir.c src/xml.c src/hwpx_package.c
    src/hwpx_read.c src/hwpx_write.c src/edit.c src/hwp.c)
target_include_directories(hangulpad_engine PUBLIC include)
target_link_libraries(hangulpad_engine PUBLIC libzip::zip)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hangulpad_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_executable(hp src/cli.c)
target_link_libraries(hp PRIVATE hangulpad_engine)
if(WIN32)
    enable_language(RC)
    set_source_files_properties(res/hangulpad.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/hp/version.h")
    target_sources(hp PRIVATE res/hangulpad.rc)
    target_link_libraries(hp PRIVATE shell32)
    add_executable(HangulPad WIN32 src/gui_win.c res/hangulpad.rc)
    target_include_directories(HangulPad PRIVATE include)
    target_link_libraries(HangulPad PRIVATE hangulpad_engine
        comdlg32 user32 gdi32 shell32 ole32 uuid windowscodecs msimg32)
    if(MINGW)
        target_link_options(HangulPad PRIVATE -municode)
    endif()
    target_include_directories(hp PRIVATE include)
    if(NOT CMAKE_CROSSCOMPILING)
        get_filename_component(runtime_toolchain_bin "${CMAKE_C_COMPILER}" DIRECTORY)
        add_custom_target(copy-runtime-dlls ALL
            COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:HangulPad>"
                "-DTOOLCHAIN_BIN=${runtime_toolchain_bin}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRuntimeDependencies.cmake"
            DEPENDS HangulPad hp
            VERBATIM COMMENT "Copying runtime DLLs beside the executables")
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(test_table_pagination tests/test_table_pagination.c)
    target_include_directories(test_table_pagination PRIVATE include)
    add_test(NAME table-pagination COMMAND test_table_pagination)
    add_executable(test_page_flow tests/test_page_flow.c)
    add_test(NAME page-flow COMMAND test_page_flow)
    add_executable(test_line_layout tests/test_line_layout.c)
    target_include_directories(test_line_layout PRIVATE include)
    add_test(NAME line-layout COMMAND test_line_layout)
    add_executable(test_edit tests/test_edit.c)
    target_link_libraries(test_edit PRIVATE hangulpad_engine)
    if(WIN32)
        target_link_libraries(test_edit PRIVATE shell32)
    endif()
    add_executable(test_roundtrip tests/test_roundtrip.c)
    target_link_libraries(test_roundtrip PRIVATE hangulpad_engine)
    add_executable(test_pagination tests/test_pagination.c)
    target_link_libraries(test_pagination PRIVATE hangulpad_engine)
    add_executable(test_xml tests/test_xml.c)
    target_link_libraries(test_xml PRIVATE hangulpad_engine)
    add_executable(irdump tests/irdump.c)
    target_link_libraries(irdump PRIVATE hangulpad_engine)
    if(WIN32)
        target_link_libraries(irdump PRIVATE shell32) # UTF-8 arguments (Korean sample names)
        add_executable(test_gui_fidelity tests/test_gui_fidelity.c)
        target_link_libraries(test_gui_fidelity PRIVATE hangulpad_engine
            comdlg32 user32 gdi32 shell32 ole32 uuid windowscodecs msimg32)
        add_test(NAME gui-fidelity COMMAND test_gui_fidelity)
        set_tests_properties(gui-fidelity PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        add_executable(test_gui_decorations tests/test_gui_decorations.c)
        target_link_libraries(test_gui_decorations PRIVATE hangulpad_engine
            comdlg32 user32 gdi32 shell32 ole32 uuid windowscodecs msimg32)
        add_test(NAME gui-decorations COMMAND test_gui_decorations)
        set_tests_properties(gui-decorations PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        add_executable(test_gui_edit tests/test_gui_edit.c)
        target_link_libraries(test_gui_edit PRIVATE hangulpad_engine
            comdlg32 user32 gdi32 shell32 ole32 uuid windowscodecs msimg32)
        add_test(NAME gui-edit COMMAND test_gui_edit)
        add_executable(test_gui_layout tests/test_gui_layout.c)
        target_link_libraries(test_gui_layout PRIVATE hangulpad_engine
            comdlg32 user32 gdi32 shell32 ole32 uuid windowscodecs msimg32)
        add_executable(layout_render tools/layout_render.c)
        target_link_libraries(layout_render PRIVATE hangulpad_engine
            comdlg32 user32 gdi32 shell32 ole32 uuid windowscodecs msimg32)
        add_test(NAME gui-layout COMMAND test_gui_layout)
        set_tests_properties(gui-layout PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    # Tests that write files run in the build tree; their inputs are refreshed from the
    # source tree on every ctest run (not only at configure time).
    set(test_work "${CMAKE_CURRENT_BINARY_DIR}/test-work")
    if(WIN32)
        set_tests_properties(gui-edit PROPERTIES ENVIRONMENT "TMP=${test_work};TEMP=${test_work}"
            FIXTURES_REQUIRED test_work)
    endif()
    add_test(NAME prepare-test-work COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures" "${test_work}/tests/fixtures")
    add_test(NAME prepare-test-tmp COMMAND "${CMAKE_COMMAND}" -E make_directory "${test_work}/tmp")
    set_tests_properties(prepare-test-work prepare-test-tmp PROPERTIES FIXTURES_SETUP test_work)
    add_test(NAME edit COMMAND test_edit)
    set(edit_tests edit)
    foreach(corpus ethics gyuje-규제합리화 mfds-프로프라놀롤)
        add_test(NAME "edit-${corpus}" COMMAND test_edit "${CMAKE_CURRENT_SOURCE_DIR}/sample/corpus/${corpus}.hwpx")
        list(APPEND edit_tests "edit-${corpus}")
    endforeach()
    set_tests_properties(${edit_tests} PROPERTIES WORKING_DIRECTORY "${test_work}"
        FIXTURES_REQUIRED test_work RESOURCE_LOCK edit_work)
    add_test(NAME xml COMMAND test_xml)
    add_test(NAME roundtrip COMMAND test_roundtrip)
    add_test(NAME pagination COMMAND test_pagination)
    set_tests_properties(edit xml roundtrip pagination PROPERTIES WORKING_DIRECTORY "${test_work}"
        FIXTURES_REQUIRED test_work)
    # Golden checks read samples and expected IR straight from the source tree.
    file(GLOB golden_inputs RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
        "sample/corpus/*.hwpx" "tests/fixtures/*.hwpx")
    foreach(input IN LISTS golden_inputs)
        get_filename_component(stem "${input}" NAME_WE)
        add_test(NAME "golden-${stem}" COMMAND "${CMAKE_COMMAND}"
            "-DPROGRAM=$<TARGET_FILE:irdump>" "-DINPUT=${input}"
            "-DEXPECTED=tests/golden/${stem}.txt" "-DWORK_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GoldenCheck.cmake")
    endforeach()
    # After an intended reader change: cmake --build <dir> --target golden-update,
    # then review the diff of tests/golden (files are written as LF, same as Linux).
    set(golden_update_cmds)
    foreach(input IN LISTS golden_inputs)
        get_filename_component(stem "${input}" NAME_WE)
        list(APPEND golden_update_cmds COMMAND "${CMAKE_COMMAND}"
            "-DPROGRAM=$<TARGET_FILE:irdump>" "-DINPUT=${input}"
            "-DOUTPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/${stem}.txt"
            "-DWORK_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GoldenUpdate.cmake")
    endforeach()
    add_custom_target(golden-update ${golden_update_cmds} DEPENDS irdump VERBATIM
        COMMENT "Regenerating tests/golden from the current reader")
endif()

if(WIN32 AND NOT CMAKE_CROSSCOMPILING)
    get_filename_component(toolchain_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    install(TARGETS hp HangulPad RUNTIME_DEPENDENCIES
        DIRECTORIES "${toolchain_bin}" "${CMAKE_PREFIX_PATH}/bin"
        PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
        POST_EXCLUDE_REGEXES [[.*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\/][Ss][Yy][Ss][Tt][Ee][Mm]32[\\/].*]]
        RUNTIME DESTINATION .)
elseif(WIN32)
    install(TARGETS hp HangulPad RUNTIME DESTINATION .)
else()
    install(TARGETS hp RUNTIME DESTINATION bin)
endif()
install(DIRECTORY sample/documents/ DESTINATION documents)
install(FILES tests/fixtures/table_pagination.hwpx DESTINATION documents)
install(DIRECTORY sample/corpus/ DESTINATION documents/corpus FILES_MATCHING PATTERN "*.hwpx")
install(FILES README.md CHANGELOG.md LICENSE LICENSE-MIT LICENSE-APACHE NOTICE DESTINATION .)
install(DIRECTORY docs/ DESTINATION docs FILES_MATCHING PATTERN "*.md")
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME "hangulpad-${HP_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-portable")
include(CPack)








