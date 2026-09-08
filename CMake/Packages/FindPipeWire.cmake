# SPDX-License-Identifier: MIT
# Copyright (C) 2026-present PortareOS (https://github.com/portare-ch)

# Finds libpipewire, for talking to the audio graph without going through the
# pulse compatibility layer.
#
# Needs libspa's include directory as well as its own: the pipewire headers
# include <spa/...> and the two ship in separate versioned directories.

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_PIPEWIRE QUIET libpipewire-0.3)
    pkg_check_modules(PC_SPA QUIET libspa-0.2)
endif()

find_path(PIPEWIRE_INCLUDE_DIR
    NAMES pipewire/pipewire.h
    HINTS ${PC_PIPEWIRE_INCLUDEDIR} ${PC_PIPEWIRE_INCLUDE_DIRS}
    PATH_SUFFIXES pipewire-0.3
)

find_path(SPA_INCLUDE_DIR
    NAMES spa/param/props.h
    HINTS ${PC_SPA_INCLUDEDIR} ${PC_SPA_INCLUDE_DIRS}
    PATH_SUFFIXES spa-0.2
)

find_library(PIPEWIRE_LIBRARY
    NAMES pipewire-0.3
    HINTS ${PC_PIPEWIRE_LIBDIR} ${PC_PIPEWIRE_LIBRARY_DIRS}
)

set(PIPEWIRE_INCLUDE_DIRS ${PIPEWIRE_INCLUDE_DIR} ${SPA_INCLUDE_DIR})
set(PIPEWIRE_LIBRARIES ${PIPEWIRE_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PipeWire
    REQUIRED_VARS PIPEWIRE_LIBRARY PIPEWIRE_INCLUDE_DIR SPA_INCLUDE_DIR
    VERSION_VAR PC_PIPEWIRE_VERSION
)

mark_as_advanced(PIPEWIRE_INCLUDE_DIR SPA_INCLUDE_DIR PIPEWIRE_LIBRARY)
