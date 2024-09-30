cmake_minimum_required(VERSION 3.10)

project(hackrf_sweeper LANGUAGES C CXX)
set(MAJOR_VERSION 0)
set(MINOR_VERSION 1)
set(PACKAGE hackrf_sweeper)
set(VERSION_STRING ${MAJOR_VERSION}.${MINOR_VERSION})
set(VERSION ${VERSION_STRING})
add_definitions(-DLIBRARY_VERSION="${VERSION_STRING}")
include(${PROJECT_SOURCE_DIR}/cmake/set_release.cmake)
add_definitions(-DLIBRARY_RELEASE="${RELEASE}")
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules)

message(STATUS "Release: ${RELEASE}")

# Set C and C++ standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Set default build type to Debug if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(PkgConfig REQUIRED)
pkg_search_module(FFTW REQUIRED fftw3 IMPORTED_TARGET)
include_directories(PkgConfig::FFTW)
link_libraries(PkgConfig::FFTW)

if(NOT libhackrf_SOURCE_DIR)
find_package(LIBHACKRF REQUIRED)
include_directories(${LIBHACKRF_INCLUDE_DIR})
else()
include_directories(${libhackrf_SOURCE_DIR}/src)
endif()

find_package(Threads REQUIRED)

find_package(FFTW REQUIRED)
include_directories(${FFTW_INCLUDES})
get_filename_component(FFTW_LIBRARY_DIRS ${FFTW_LIBRARIES} DIRECTORY)
link_directories(${FFTW_LIBRARY_DIRS})

include_directories(${FFTW_INCLUDES} ${LIBHACKRF_INCLUDE_DIRS} ${PROJECT_SOURCE_DIR}/include)

if(MSVC)
        LIST(APPEND SWEEPER_LIB_LINK_LIBS ${FFTW_LIBRARIES})
else()
        LIST(APPEND SWEEPER_LIB_LINK_LIBS m fftw3f)

        set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -Wall")
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Wall")

        set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -ggdb -Wall")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -ggdb -Wall")
endif()

# Library (libhackrf_sweeper)
add_library(libhackrf_sweeper STATIC src/lib/sweep.c)
target_link_libraries(libhackrf_sweeper ${SWEEPER_LIB_LINK_LIBS} ${LIBHACKRF_LIBRARIES})

SET(TOOLS
        hackrf_sweeper
)

if(MSVC)
	add_library(libgetopt_static STATIC
	    ../getopt/getopt.c
	)
	LIST(APPEND TOOLS_LINK_LIBS ${FFTW_LIBRARIES})
else()
    LIST(APPEND TOOLS_LINK_LIBS m fftw3f ${CMAKE_THREAD_LIBS_INIT})
endif()

if(NOT libhackrf_SOURCE_DIR)
	include_directories(${LIBHACKRF_INCLUDE_DIR})
	LIST(APPEND TOOLS_LINK_LIBS ${LIBHACKRF_LIBRARIES})
else()
	LIST(APPEND TOOLS_LINK_LIBS hackrf)
endif()

if(MSVC)
	LIST(APPEND TOOLS_LINK_LIBS libgetopt_static)
endif()

foreach(tool ${TOOLS})
	add_executable(${tool} src/tools/${tool}.c)
	target_link_libraries(${tool} ${TOOLS_LINK_LIBS} libhackrf_sweeper)
	install(TARGETS ${tool} RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})
endforeach(tool)

if( ${WIN32} )
	install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIGURATION>/"
        	DESTINATION ${INSTALL_DEFAULT_BINDIR}
			FILES_MATCHING
			PATTERN "fftw*.dll")
endif( ${WIN32} )

if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CZMQ libczmq)
    pkg_check_modules(LZMQ libzmq)


     # Find MessagePack
     find_package(Msgpack REQUIRED)

     if(CZMQ_FOUND AND Msgpack_FOUND AND LZMQ_FOUND)
        message(STATUS "CZeroMQ found: ${CZMQ_LIBRARIES}")
        message(STATUS "Msgpack found: ${MSGPACK_LIBRARIES}")
        message(STATUS "LibZMQ found: ${LZMQ_LIBRARIES}")

        add_executable(hackrf_sweeper_zmqpub src/tools/hackrf_sweeper_zmqpub.c)
        # Include directories for MessagePack
        include_directories(${MSGPACK_INCLUDE_DIRS} ${CZMQ_INCLUDE_DIRS} ${LZMQ_INCLUDE_DIRS})

        target_link_libraries(hackrf_sweeper_zmqpub ${TOOLS_LINK_LIBS} ${CZMQ_LIBRARIES} ${LZMQ_LIBRARIES} libhackrf_sweeper pthread)
        install(TARGETS hackrf_sweeper_zmqpub RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})
    else()
        message(WARNING "ZeroMQ not found. hackrf_sweeper_zmqpub will not be built.")
    endif()

endif()

install(TARGETS libhackrf_sweeper hackrf_sweeper
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)

install(DIRECTORY include/ DESTINATION include)

# Summary of the project configuration
message(STATUS "Using FFTW include directory: ${FFTW_INCLUDE_DIRS}")
message(STATUS "Using libhackrf include directory: ${LIBHACKRF_INCLUDE_DIRS}")
