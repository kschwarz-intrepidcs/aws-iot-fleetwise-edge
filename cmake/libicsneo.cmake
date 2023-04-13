#
#  Tries to find libicsneo headers and libraries.
#
#  Usage of this module as follows:
#
#  find_package(libicsneo)
#
#  Variables used by this module:
#
#  Variables defined by this module:
#
#  LIBICSNEO_LIBRARIES          The libicsneo libraries
#  LIBICSNEO_INCLUDE_DIR        The location of libicsneo headers

find_path(LIBICSNEO_INCLUDE_DIR
    NAMES icsneo/icsneocpp.h
)

find_library(LIBICSNEO_ICSNEOCPP
    NAMES icsneocpp
)

find_library(LIBICSNEO_FATFS
    NAMES fatfs
)

set(LIBICSNEO_LIBRARIES ${LIBICSNEO_ICSNEOCPP} ${LIBICSNEO_FATFS})

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(libicsneo
    DEFAULT_MSG
    LIBICSNEO_INCLUDE_DIR
    LIBICSNEO_LIBRARIES
)
