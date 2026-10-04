
# Try to find the OpenXR loader library and include path.
# Once done this will define
#
# OPENXR_FOUND
# OpenXR_INCLUDE_DIR   Where to find openxr/openxr.h
# OpenXR_LIBRARIES     The OpenXR loader library
#
# Typical usage could be something like:
#   find_package(OpenXR REQUIRED)
#   include_directories(SYSTEM ${OpenXR_INCLUDE_DIR})
#   ...
#   target_link_libraries(myexe ${OpenXR_LIBRARIES})

if(UNIX)
	find_package(PkgConfig QUIET)
	pkg_check_modules(_PC_OpenXR openxr)
endif()

find_path(OpenXR_INCLUDE_DIR openxr/openxr.h
	HINTS
		${_PC_OpenXR_INCLUDE_DIRS}
	PATHS
		/usr/include
		/usr/local/include
		/opt/local/include
	DOC "The directory where openxr/openxr.h resides"
)
mark_as_advanced(OpenXR_INCLUDE_DIR)

# Prefer libraries in the same prefix as the include files
string(REGEX REPLACE "(.*)/include/?" "\\1" OpenXR_BASE_DIR "${OpenXR_INCLUDE_DIR}")

find_library(OpenXR_LIBRARY
	NAMES openxr_loader
	HINTS
		${_PC_OpenXR_LIBRARY_DIRS}
		"${OpenXR_BASE_DIR}/lib"
	PATHS
		/usr/lib64
		/usr/lib
		/usr/local/lib64
		/usr/local/lib
		/opt/local/lib
	DOC "The OpenXR loader library"
)
mark_as_advanced(OpenXR_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set OPENXR_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OpenXR DEFAULT_MSG OpenXR_LIBRARY OpenXR_INCLUDE_DIR)

if(OPENXR_FOUND)
	set(OpenXR_LIBRARIES ${OpenXR_LIBRARY})
endif(OPENXR_FOUND)
