include(FindPackageHandleStandardArgs)

set(_LibObs_SEARCH_ROOTS)
foreach(_var LIBOBS_ROOT OBS_ROOT OBS_STUDIO_DIR)
    if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
        list(APPEND _LibObs_SEARCH_ROOTS "${${_var}}")
    endif()
    if(DEFINED ENV{${_var}} AND NOT "$ENV{${_var}}" STREQUAL "")
        list(APPEND _LibObs_SEARCH_ROOTS "$ENV{${_var}}")
    endif()
endforeach()

if(DEFINED LIBOBS_INCLUDE_DIR AND NOT "${LIBOBS_INCLUDE_DIR}" STREQUAL "")
    set(LibObs_INCLUDE_DIR "${LIBOBS_INCLUDE_DIR}" CACHE PATH "libobs include directory")
elseif(DEFINED ENV{LIBOBS_INCLUDE_DIR} AND NOT "$ENV{LIBOBS_INCLUDE_DIR}" STREQUAL "")
    set(LibObs_INCLUDE_DIR "$ENV{LIBOBS_INCLUDE_DIR}" CACHE PATH "libobs include directory")
else()
    find_path(LibObs_INCLUDE_DIR
        NAMES obs.h
        PATHS ${_LibObs_SEARCH_ROOTS}
        PATH_SUFFIXES include include/obs include/libobs libobs libobs/include usr/include/obs usr/include/libobs
    )
endif()

if(DEFINED LIBOBS_LIBRARY AND NOT "${LIBOBS_LIBRARY}" STREQUAL "")
    set(LibObs_LIBRARY "${LIBOBS_LIBRARY}" CACHE FILEPATH "libobs library")
elseif(DEFINED ENV{LIBOBS_LIBRARY} AND NOT "$ENV{LIBOBS_LIBRARY}" STREQUAL "")
    set(LibObs_LIBRARY "$ENV{LIBOBS_LIBRARY}" CACHE FILEPATH "libobs library")
else()
    find_library(LibObs_LIBRARY
        NAMES obs libobs
        PATHS ${_LibObs_SEARCH_ROOTS}
        PATH_SUFFIXES
            lib lib64 bin bin/64bit Frameworks Frameworks/libobs.framework
            usr/lib usr/lib64 usr/lib/x86_64-linux-gnu usr/lib/aarch64-linux-gnu
            build/libobs build/libobs/Release
    )
endif()

if(DEFINED LIBOBS_RUNTIME_LIBRARY AND NOT "${LIBOBS_RUNTIME_LIBRARY}" STREQUAL "")
    set(LibObs_RUNTIME_LIBRARY "${LIBOBS_RUNTIME_LIBRARY}" CACHE FILEPATH "libobs runtime library")
elseif(DEFINED ENV{LIBOBS_RUNTIME_LIBRARY} AND NOT "$ENV{LIBOBS_RUNTIME_LIBRARY}" STREQUAL "")
    set(LibObs_RUNTIME_LIBRARY "$ENV{LIBOBS_RUNTIME_LIBRARY}" CACHE FILEPATH "libobs runtime library")
endif()

if(WIN32 AND LibObs_LIBRARY)
    get_filename_component(_LibObs_LIBRARY_EXT "${LibObs_LIBRARY}" EXT)
    string(TOLOWER "${_LibObs_LIBRARY_EXT}" _LibObs_LIBRARY_EXT_LC)
    if(_LibObs_LIBRARY_EXT_LC STREQUAL ".dll")
        if(NOT LibObs_RUNTIME_LIBRARY)
            set(LibObs_RUNTIME_LIBRARY "${LibObs_LIBRARY}" CACHE FILEPATH "libobs runtime library" FORCE)
        endif()

        set(_LibObs_CANDIDATE_IMPLIBS)
        if(DEFINED LIBOBS_LIBRARY AND NOT "${LIBOBS_LIBRARY}" STREQUAL "" AND NOT "${LIBOBS_LIBRARY}" MATCHES "\\.dll$")
            list(APPEND _LibObs_CANDIDATE_IMPLIBS "${LIBOBS_LIBRARY}")
        endif()
        if(DEFINED ENV{LIBOBS_LIBRARY} AND NOT "$ENV{LIBOBS_LIBRARY}" STREQUAL "" AND NOT "$ENV{LIBOBS_LIBRARY}" MATCHES "\\.dll$")
            list(APPEND _LibObs_CANDIDATE_IMPLIBS "$ENV{LIBOBS_LIBRARY}")
        endif()
        if(DEFINED ENV{GITHUB_WORKSPACE})
            list(APPEND _LibObs_CANDIDATE_IMPLIBS "$ENV{GITHUB_WORKSPACE}/.ci/obs/link/obs.lib")
        endif()
        foreach(_LibObs_IMPLIB_CANDIDATE IN LISTS _LibObs_CANDIDATE_IMPLIBS)
            if(EXISTS "${_LibObs_IMPLIB_CANDIDATE}")
                set(LibObs_LIBRARY "${_LibObs_IMPLIB_CANDIDATE}" CACHE FILEPATH "libobs import library" FORCE)
                break()
            endif()
        endforeach()

        get_filename_component(_LibObs_LIBRARY_EXT "${LibObs_LIBRARY}" EXT)
        string(TOLOWER "${_LibObs_LIBRARY_EXT}" _LibObs_LIBRARY_EXT_LC)
        if(_LibObs_LIBRARY_EXT_LC STREQUAL ".dll")
            message(FATAL_ERROR "LibObs_LIBRARY resolved to obs.dll. On Windows it must be the generated obs.lib import library; set LIBOBS_LIBRARY to .ci/obs/link/obs.lib and LIBOBS_RUNTIME_LIBRARY to the private obs.dll.")
        endif()
    endif()
endif()

find_package_handle_standard_args(LibObs
    REQUIRED_VARS LibObs_LIBRARY LibObs_INCLUDE_DIR
)

if(LibObs_FOUND AND NOT TARGET LibObs::libobs)
    # On Windows we deliberately expose libobs as an INTERFACE target that links
    # only the import library.  Imported SHARED/UNKNOWN targets have proven too
    # easy to misconfigure so link.exe receives obs.dll and fails with LNK1107.
    # The DLL path is kept only as metadata for runtime extraction/delay-load.
    if(WIN32)
        add_library(LibObs::libobs INTERFACE IMPORTED)
    else()
        add_library(LibObs::libobs UNKNOWN IMPORTED)
    endif()

    set(_LibObs_INTERFACE_INCLUDE_DIRS "${LibObs_INCLUDE_DIR}")
    get_filename_component(_LibObs_INCLUDE_PARENT "${LibObs_INCLUDE_DIR}" DIRECTORY)
    if(EXISTS "${_LibObs_INCLUDE_PARENT}/simde/x86/sse2.h")
        list(APPEND _LibObs_INTERFACE_INCLUDE_DIRS "${_LibObs_INCLUDE_PARENT}")
    endif()

    set(_LibObs_INTERFACE_LINK_OPTIONS)
    if(UNIX AND NOT APPLE)
        set(_LibObs_RUNTIME_ROOT "")
        if(DEFINED VODLINK_OBS_RUNTIME_DIR AND NOT "${VODLINK_OBS_RUNTIME_DIR}" STREQUAL "")
            set(_LibObs_RUNTIME_ROOT "${VODLINK_OBS_RUNTIME_DIR}")
        elseif(DEFINED ENV{VODLINK_OBS_RUNTIME_DIR} AND NOT "$ENV{VODLINK_OBS_RUNTIME_DIR}" STREQUAL "")
            set(_LibObs_RUNTIME_ROOT "$ENV{VODLINK_OBS_RUNTIME_DIR}")
        endif()

        if(_LibObs_RUNTIME_ROOT)
            foreach(_LibObs_RPATH_LINK_DIR
                    "${_LibObs_RUNTIME_ROOT}/usr/lib/x86_64-linux-gnu"
                    "${_LibObs_RUNTIME_ROOT}/usr/lib/aarch64-linux-gnu"
                    "${_LibObs_RUNTIME_ROOT}/usr/lib"
                    "${_LibObs_RUNTIME_ROOT}/usr/local/lib/x86_64-linux-gnu"
                    "${_LibObs_RUNTIME_ROOT}/usr/local/lib/aarch64-linux-gnu"
                    "${_LibObs_RUNTIME_ROOT}/usr/local/lib"
                    "${_LibObs_RUNTIME_ROOT}/lib/x86_64-linux-gnu"
                    "${_LibObs_RUNTIME_ROOT}/lib/aarch64-linux-gnu"
                    "${_LibObs_RUNTIME_ROOT}/lib"
                    "${_LibObs_RUNTIME_ROOT}")
                if(EXISTS "${_LibObs_RPATH_LINK_DIR}")
                    list(APPEND _LibObs_INTERFACE_LINK_OPTIONS "-Wl,-rpath-link,${_LibObs_RPATH_LINK_DIR}")
                endif()
            endforeach()
        endif()
    endif()

    if(WIN32)
        get_filename_component(_LibObs_FINAL_LINK_EXT "${LibObs_LIBRARY}" EXT)
        string(TOLOWER "${_LibObs_FINAL_LINK_EXT}" _LibObs_FINAL_LINK_EXT_LC)
        if(_LibObs_FINAL_LINK_EXT_LC STREQUAL ".dll")
            message(FATAL_ERROR "Refusing to link against obs.dll. LibObs_LIBRARY must point to obs.lib; LibObs_RUNTIME_LIBRARY must point to obs.dll.")
        endif()
        if(NOT LibObs_RUNTIME_LIBRARY)
            message(FATAL_ERROR "LIBOBS_RUNTIME_LIBRARY must point to VodLink's private obs.dll on Windows.")
        endif()
        set_target_properties(LibObs::libobs PROPERTIES
            INTERFACE_LINK_LIBRARIES "${LibObs_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${_LibObs_INTERFACE_INCLUDE_DIRS}"
            INTERFACE_LINK_OPTIONS "${_LibObs_INTERFACE_LINK_OPTIONS}"
            VODLINK_LIBOBS_RUNTIME_LIBRARY "${LibObs_RUNTIME_LIBRARY}"
        )
    else()
        set_target_properties(LibObs::libobs PROPERTIES
            IMPORTED_LOCATION "${LibObs_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${_LibObs_INTERFACE_INCLUDE_DIRS}"
            INTERFACE_LINK_OPTIONS "${_LibObs_INTERFACE_LINK_OPTIONS}"
        )
    endif()
endif()

mark_as_advanced(LibObs_INCLUDE_DIR LibObs_LIBRARY LibObs_RUNTIME_LIBRARY)
