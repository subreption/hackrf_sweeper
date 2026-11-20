cmake_minimum_required(VERSION 3.13)

project(hackrf_sweeper LANGUAGES C CXX)
set(MAJOR_VERSION 0)
set(MINOR_VERSION 1)
set(PACKAGE hackrf_sweeper)
set(VERSION_STRING ${MAJOR_VERSION}.${MINOR_VERSION})
set(VERSION ${VERSION_STRING})
include(${PROJECT_SOURCE_DIR}/cmake/set_release.cmake)
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules)

message(STATUS "Release: ${RELEASE}")

# Set C and C++ standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Set default build type to Debug if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

if(NOT libhackrf_SOURCE_DIR)
find_package(LIBHACKRF REQUIRED)
endif()

find_package(Threads REQUIRED)

find_package(FFTW REQUIRED)
get_filename_component(FFTW_LIBRARY_DIRS ${FFTW_LIBRARIES} DIRECTORY)

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
add_library(hackrf_sweeper STATIC src/lib/sweep.c)
target_compile_definitions(hackrf_sweeper PRIVATE LIBRARY_VERSION="${VERSION_STRING}" LIBRARY_RELEASE="${RELEASE}")
target_include_directories(hackrf_sweeper PUBLIC ${PROJECT_SOURCE_DIR}/include ${FFTW_INCLUDES})
if(NOT libhackrf_SOURCE_DIR)
	target_include_directories(hackrf_sweeper PUBLIC ${LIBHACKRF_INCLUDE_DIR})
else()
	target_include_directories(hackrf_sweeper PUBLIC ${libhackrf_SOURCE_DIR}/src)
endif()
target_link_directories(hackrf_sweeper PUBLIC ${FFTW_LIBRARY_DIRS})
target_link_libraries(hackrf_sweeper PUBLIC ${SWEEPER_LIB_LINK_LIBS} ${LIBHACKRF_LIBRARIES})

SET(TOOLS
    # empty for now
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
	LIST(APPEND TOOLS_LINK_LIBS ${LIBHACKRF_LIBRARIES})
else()
	LIST(APPEND TOOLS_LINK_LIBS hackrf)
endif()

if(MSVC)
	LIST(APPEND TOOLS_LINK_LIBS libgetopt_static)
endif()

# hackrf_sweep replacement (handled out of band to avoid name collisions with the lib)
add_executable(hackrf_sweeper_tool src/tools/hackrf_sweeper.c)
target_link_libraries(hackrf_sweeper_tool PRIVATE ${TOOLS_LINK_LIBS} hackrf_sweeper)
install(TARGETS hackrf_sweeper_tool RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})

# Process any other tools
foreach(tool ${TOOLS})
	add_executable(${tool} src/tools/${tool}.c)
	target_link_libraries(${tool} PRIVATE ${TOOLS_LINK_LIBS} hackrf_sweeper)
	install(TARGETS ${tool} RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})
endforeach(tool)

if( ${WIN32} )
	install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIGURATION>/"
        	DESTINATION ${INSTALL_DEFAULT_BINDIR}
			FILES_MATCHING
			PATTERN "fftw*.dll")
endif( ${WIN32} )

# Handle the ZMQ publisher demo separately
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CZMQ libczmq)
    pkg_check_modules(LZMQ libzmq)


     # Find MessagePack
     find_package(msgpack REQUIRED)

     if(CZMQ_FOUND AND msgpack_FOUND AND LZMQ_FOUND)
        message(STATUS "CZeroMQ found: ${CZMQ_LIBRARIES}")
        message(STATUS "Msgpack found: ${MSGPACK_LIBRARIES}")
        message(STATUS "LibZMQ found: ${LZMQ_LIBRARIES}")

        add_executable(hackrf_sweeper_zmqpub src/tools/hackrf_sweeper_zmqpub.c)
        # Include directories for MessagePack
        target_include_directories(hackrf_sweeper_zmqpub PRIVATE ${MSGPACK_INCLUDE_DIRS} ${CZMQ_INCLUDE_DIRS} ${LZMQ_INCLUDE_DIRS})

        target_link_libraries(hackrf_sweeper_zmqpub PRIVATE ${TOOLS_LINK_LIBS} ${CZMQ_LIBRARIES} ${LZMQ_LIBRARIES} hackrf_sweeper pthread)
        install(TARGETS hackrf_sweeper_zmqpub RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})
    else()
        message(WARNING "hackrf_sweeper_zmqpub will not be built, missing deps.")
    endif()

endif()

install(TARGETS hackrf_sweeper hackrf_sweeper_tool
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)

install(DIRECTORY include/ DESTINATION include)

# Summary of the project configuration
message(STATUS "Using FFTW include directory: ${FFTW_INCLUDE_DIRS}")
message(STATUS "Using libhackrf include directory: ${LIBHACKRF_INCLUDE_DIRS}")
