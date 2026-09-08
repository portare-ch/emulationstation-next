# Finds libmpv, the client library of the mpv media player.
#
# MPV_FOUND, MPV_INCLUDE_DIR and MPV_LIBRARIES are set, matching the names the
# module it replaced used, so the call sites in CMakeLists.txt read the same way.

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_MPV QUIET mpv)
endif()

find_path(MPV_INCLUDE_DIR
    NAMES mpv/client.h
    HINTS ${PC_MPV_INCLUDEDIR} ${PC_MPV_INCLUDE_DIRS}
)

find_library(MPV_LIBRARIES
    NAMES mpv
    HINTS ${PC_MPV_LIBDIR} ${PC_MPV_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MPV
    REQUIRED_VARS MPV_LIBRARIES MPV_INCLUDE_DIR
    VERSION_VAR PC_MPV_VERSION
)

mark_as_advanced(MPV_INCLUDE_DIR MPV_LIBRARIES)
