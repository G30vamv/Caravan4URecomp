# The native port of Monster Hunter 4 Ultimate (docs/native/ROADMAP.md).
#
# Nothing in this file needs editing to add a module, a tool, a test or a registration:
#   native/src/**/CMakeLists.txt   a module (mh4n_module), found at any depth
#   native/tools/<name>/           a tool (mh4n_program), found by its CMakeLists.txt
#   native/tests/<name>_test.cpp   a doctest binary, found by its name
#   native/runner/                 the runner
# See docs/native/wp/N1.md (rules for code in native/).
cmake_minimum_required(VERSION 3.25)
project(mh4n C CXX)

# MinGW (llvm-mingw's lld): CMake's Windows-GNU module decides between --push-state/--pop-state and
# plain --whole-archive by asking CMAKE_LINKER (a GNU ld or none), not the linker lld that will run,
# and lld's MinGW driver has no --push-state. Plain --whole-archive/--no-whole-archive is enough.
if (WIN32 AND NOT MSVC)
    set(CMAKE_LINK_LIBRARY_USING_WHOLE_ARCHIVE "LINKER:--whole-archive" "<LINK_ITEM>" "LINKER:--no-whole-archive")
    set(CMAKE_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE)
    # CMake 3.31 also sets the per-language variables, which come first: the same for them
    foreach (lang C CXX)
        set(CMAKE_${lang}_LINK_LIBRARY_USING_WHOLE_ARCHIVE "LINKER:--whole-archive" "<LINK_ITEM>" "LINKER:--no-whole-archive")
        set(CMAKE_${lang}_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE)
    endforeach()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(MH4N_TESTS "Build the unit tests (native/tests)" ON)
option(MH4N_WERROR "Treat warnings in our own code as errors" OFF)
option(MH4N_ASAN "Build with AddressSanitizer" OFF)
option(MH4N_DEBUG_GUARD "Guest memory debug guard (N1-02)" OFF)
set(MH4N_OWN_MODULES "")
foreach (own_module small_services cfg_ptm y2r ir fs apt dsp_core dsp_service)
    list(APPEND MH4N_OWN_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/own/${own_module}/module")
endforeach()
set(MH4N_EXTRA_MODULES "${MH4N_OWN_MODULES}" CACHE STRING "Directories of extra modules, ';'-separated: by default the independent services and the native DSP under own/ (Windows port)")
if (NOT MH4N_EXTRA_MODULES)
    set(MH4N_EXTRA_MODULES "${MH4N_OWN_MODULES}")  # an older cache entry that is empty means the default too
endif()
if (MH4N_EXTRA_MODULES STREQUAL "OFF")
    set(MH4N_EXTRA_MODULES "")  # -DMH4N_EXTRA_MODULES=OFF: none of the independent services (tests of the older ones)
endif()
option(MH4N_APP"Build the app tree native/app (SDL3, Dear ImGui: mh4u-native, N2b-03)" OFF)

# Programs go to <build>/bin.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(MH4N_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(MH4N_SRC_DIR "${MH4N_ROOT}/src")

if (MH4N_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
add_compile_definitions(MH4N_DEBUG_GUARD=$<BOOL:${MH4N_DEBUG_GUARD}>)

find_package(Threads REQUIRED)

# Warnings for our own targets (third-party code does not link this).
add_library(mh4n_warnings INTERFACE)
if (MSVC)
    target_compile_options(mh4n_warnings INTERFACE /W4 $<$<BOOL:${MH4N_WERROR}>:/WX>)
else()
    target_compile_options(mh4n_warnings INTERFACE -Wall -Wextra
                           $<$<BOOL:${MH4N_WERROR}>:-Werror>)
endif()

# The public headers (the contracts), included as <mh4n/name.h>.
add_library(mh4n_headers INTERFACE)
target_include_directories(mh4n_headers INTERFACE "${MH4N_ROOT}/include")

# ---- third party (native/THIRD_PARTY.md) -------------------------------------------------------
add_library(mh4n_fmt STATIC third_party/fmt/src/format.cc)
target_include_directories(mh4n_fmt SYSTEM PUBLIC third_party/fmt/include)
target_compile_options(mh4n_fmt PRIVATE $<IF:$<BOOL:${MSVC}>,/w,-w>)
add_library(fmt::fmt ALIAS mh4n_fmt)

add_library(mh4n_doctest INTERFACE)
target_include_directories(mh4n_doctest SYSTEM INTERFACE third_party/doctest)
add_library(doctest::doctest ALIAS mh4n_doctest)

# ---- helpers -----------------------------------------------------------------------------------
# mh4n_module(SOURCES a.cpp ... [DEPENDS mh4n_x ...] [THIRD_PARTY target ...])
#
# Called from native/src/<path>/CMakeLists.txt; defines the static library mh4n_<path with '_'
# for '/'>. DEPENDS names modules by their full library name. Modules (and mh4n_common, always)
# are linked through the WHOLE_ARCHIVE feature so that static registrations survive the linker;
# third-party targets are linked normally.
set_property(GLOBAL PROPERTY MH4N_MODULES "")

function(mh4n_module)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "" "SOURCES;DEPENDS;THIRD_PARTY")
    if (ARG_UNPARSED_ARGUMENTS OR ARG_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "mh4n_module: bad arguments '${ARG_UNPARSED_ARGUMENTS}' "
                            "'${ARG_KEYWORDS_MISSING_VALUES}' in ${CMAKE_CURRENT_LIST_DIR}")
    endif()
    file(RELATIVE_PATH rel "${MH4N_SRC_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    # MODIFIED for the Windows port (Phase 6, independent implementations): a module may also live outside
    # native/src/, in a directory named by MH4N_EXTRA_MODULES (empty by default: nothing changes); the
    # loop below sets MH4N_EXTRA_CURRENT to its name and the library is mh4n_x_<name>.
    if (MH4N_EXTRA_CURRENT)
        set(rel "x/${MH4N_EXTRA_CURRENT}")
    endif()
    if (rel STREQUAL "" OR rel MATCHES "^\\.\\.")
        message(FATAL_ERROR "mh4n_module is for directories under native/src/ "
                            "(${CMAKE_CURRENT_SOURCE_DIR})")
    endif()
    if (NOT ARG_SOURCES)
        message(FATAL_ERROR "mh4n_module: ${rel} has no SOURCES")
    endif()
    string(REPLACE "/" "_" suffix "${rel}")
    set(name "mh4n_${suffix}")

    # MODIFIED for the Windows port (Phase 6, independent implementations; nothing is set by default, so nothing changes):
    # MH4N_REPLACE_<MODULE> (the module's directory under src/ in upper case, '/' written '_': IPC, MEMORY, SERVICES_GSP, GPU_TEXTURE...)
    # names a directory whose *.cpp replace the sources of this module with the same file name; a new name is added. The module's own
    # directory goes on its include path, so the replacement files include its internal headers as "name.h".
    string(TOUPPER "${suffix}" replace_key)
    if (MH4N_REPLACE_${replace_key})
        file(GLOB replacement_sources CONFIGURE_DEPENDS "${MH4N_REPLACE_${replace_key}}/*.cpp")
        set(replaced_sources "")
        foreach (source IN LISTS ARG_SOURCES)
            get_filename_component(source_name "${source}" NAME)
            set(is_replaced FALSE)
            foreach (replacement IN LISTS replacement_sources)
                get_filename_component(replacement_name "${replacement}" NAME)
                if (replacement_name STREQUAL source_name)
                    set(is_replaced TRUE)
                endif()
            endforeach()
            if (NOT is_replaced)
                list(APPEND replaced_sources "${source}")
            endif()
        endforeach()
        foreach (replacement IN LISTS replacement_sources)
            get_filename_component(replacement_name "${replacement}" NAME)
            message(STATUS "${name}: ${replacement_name} from ${MH4N_REPLACE_${replace_key}}")
            list(APPEND replaced_sources "${replacement}")
        endforeach()
        set(ARG_SOURCES ${replaced_sources})
        set(replace_include_dir "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    # the module's own directory is on its include path (the independent implementations include their internal headers as "name.h")
    set(replace_include_dir "${CMAKE_CURRENT_SOURCE_DIR}")
    add_library(${name} STATIC ${ARG_SOURCES})
    if (replace_include_dir)
        target_include_directories(${name} PRIVATE "${replace_include_dir}")
    endif()
    target_link_libraries(${name} PRIVATE mh4n_warnings)
    target_link_libraries(${name} PUBLIC mh4n_headers)

    set(deps ${ARG_DEPENDS})
    if (NOT name STREQUAL "mh4n_common")
        list(PREPEND deps mh4n_common)
    endif()
    list(REMOVE_DUPLICATES deps)
    foreach (dep IN LISTS deps)
        if (dep STREQUAL name)
            message(FATAL_ERROR "mh4n_module: ${name} depends on itself")
        endif()
        target_link_libraries(${name} PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,${dep}>")
    endforeach()
    if (ARG_THIRD_PARTY)
        target_link_libraries(${name} PUBLIC ${ARG_THIRD_PARTY})
    endif()

    set_property(GLOBAL APPEND PROPERTY MH4N_MODULES ${name})
    set_property(GLOBAL APPEND PROPERTY MH4N_MODULE_DEPS ${deps})
endfunction()

# mh4n_program(<name> SOURCES a.cpp ... [LINK libs...])
#
# An executable that links every module (mh4n::all). For native/runner/ and native/tools/<name>/;
# their logic belongs in modules, so that tests can link it.
function(mh4n_program name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;LINK")
    if (ARG_UNPARSED_ARGUMENTS OR NOT ARG_SOURCES)
        message(FATAL_ERROR "mh4n_program(${name}): needs SOURCES")
    endif()
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE mh4n::all mh4n_warnings ${ARG_LINK})
endfunction()

# ---- modules -----------------------------------------------------------------------------------
file(GLOB_RECURSE module_lists CONFIGURE_DEPENDS LIST_DIRECTORIES false
     "${MH4N_SRC_DIR}/*/CMakeLists.txt")
list(SORT module_lists)
foreach (list_file IN LISTS module_lists)
    get_filename_component(module_dir "${list_file}" DIRECTORY)
    file(RELATIVE_PATH module_rel "${MH4N_SRC_DIR}" "${module_dir}")
    add_subdirectory("${module_dir}" "${CMAKE_BINARY_DIR}/src/${module_rel}")
endforeach()

# MODIFIED for the Windows port: the extra modules (MH4N_EXTRA_MODULES, see mh4n_module above)
foreach (module_dir IN LISTS MH4N_EXTRA_MODULES)
    # <work package>/module: the library is named after the work package
    cmake_path(GET module_dir PARENT_PATH module_parent)
    cmake_path(GET module_parent FILENAME module_name)
    set(MH4N_EXTRA_CURRENT "${module_name}")
    add_subdirectory("${module_dir}" "${CMAKE_BINARY_DIR}/src/x/${module_name}")
    unset(MH4N_EXTRA_CURRENT)
endforeach()

get_property(MH4N_ALL_MODULES GLOBAL PROPERTY MH4N_MODULES)
get_property(MH4N_ALL_DEPS GLOBAL PROPERTY MH4N_MODULE_DEPS)
list(REMOVE_DUPLICATES MH4N_ALL_DEPS)
foreach (dep IN LISTS MH4N_ALL_DEPS)
    if (NOT dep IN_LIST MH4N_ALL_MODULES)
        message(FATAL_ERROR "A module depends on '${dep}', which is not a module under native/src/ "
                            "(modules: ${MH4N_ALL_MODULES})")
    endif()
endforeach()

# Every module, linked whole.
add_library(mh4n_all INTERFACE)
add_library(mh4n::all ALIAS mh4n_all)
foreach (module IN LISTS MH4N_ALL_MODULES)
    target_link_libraries(mh4n_all INTERFACE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${module}>")
endforeach()

# ---- the git revision, for --version -----------------------------------------------------------
set(MH4N_VERSION_HEADER "${CMAKE_BINARY_DIR}/generated/mh4n_version.h")
add_custom_target(mh4n_version
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${MH4N_ROOT}" "-DOUT=${MH4N_VERSION_HEADER}"
            -P "${MH4N_ROOT}/cmake/GitRevision.cmake"
    BYPRODUCTS "${MH4N_VERSION_HEADER}"
    COMMENT "Recording the git revision"
    VERBATIM)

# ---- programs and tests ------------------------------------------------------------------------
if (MH4N_TESTS)
    enable_testing()
endif()

add_subdirectory(runner)

file(GLOB tool_lists CONFIGURE_DEPENDS LIST_DIRECTORIES false "${MH4N_ROOT}/tools/*/CMakeLists.txt")
list(SORT tool_lists)
foreach (list_file IN LISTS tool_lists)
    get_filename_component(tool_dir "${list_file}" DIRECTORY)
    get_filename_component(tool_name "${tool_dir}" NAME)
    add_subdirectory("${tool_dir}" "${CMAKE_BINARY_DIR}/tools/${tool_name}")
endforeach()

if (MH4N_TESTS)
    add_subdirectory(tests)
endif()

# The app tree (N2b-03): SDL3, Dear ImGui and the app's shared sources. Only mh4u-native and the
# tests under native/app/tests link them; nothing above this line needs SDL3.
if (MH4N_APP)
    find_package(SDL3 3.4.18 CONFIG REQUIRED)
    add_subdirectory(app)
endif()
