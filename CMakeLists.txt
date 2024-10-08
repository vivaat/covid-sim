# CMakeLists.txt

# CMake setup
cmake_minimum_required (VERSION 3.11)

# Force Mac OS to use a compiler that can compile multi-threaded.

if(APPLE)
  set(CMAKE_C_COMPILER "/usr/local/opt/llvm/bin/clang")
  set(CMAKE_CXX_COMPILER "/usr/local/opt/llvm/bin/clang++")
endif()

# Project initialisation
project("CovidSim")

# Work around some policy behaviours
if(POLICY CMP0076)
  cmake_policy(SET CMP0076 NEW)
endif()

# Set a default build type if none was specified
set(default_build_type "RelWithDebInfo")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
  set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE
      STRING "Choose the type of build." FORCE)
  # Set the possible values of build type for cmake-gui
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

option(USE_OPENMP "Compile with OpenMP parallelism enabled" ON)

# Packages used
if(USE_OPENMP)
# Kludge to get openmp working with clang using cmake on Visual Studio.
  if(WIN32 AND (CMAKE_C_COMPILER_ID MATCHES "Clang"))
    set(OpenMP_CXX_FLAGS "-Xclang -fopenmp")
    set(OpenMP_C_FLAGS "-Xclang -fopenmp")
  endif()
   find_package(OpenMP REQUIRED)
endif()

# Python3 needed for testing
if (CMAKE_VERSION VERSION_LESS 3.12)
  set(Python_ADDITIONAL_VERSIONS 3 3.9 3.8 3.7 3.6)
  find_package(PythonInterp REQUIRED)
  set(Python3_EXECUTABLE "${PYTHON_EXECUTABLE}")
else()
  find_package(Python3 REQUIRED)
endif()

# Specify the C++ standard
set(CMAKE_CXX_STANDARD 14)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(include)
add_subdirectory(src)

enable_testing()
add_subdirectory(unit_tests)
add_subdirectory(tests)

# First we can indicate the documentation build as an option and set it to OFF by default
option(BUILD_DOC "Build documentation" OFF)

# Check if Doxygen is installed
find_package(Doxygen)
if(DOXYGEN_FOUND)
  # Set input and output files
  set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in)
  set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

  # Set whether or not to make diagrams
  set(HaveDot NO)
  set(DotPath)
  if(DOXYGEN_DOT_FOUND)
    set(HaveDot YES)
    get_filename_component(DotPath ${DOXYGEN_DOT_EXECUTABLE} DIRECTORY)
    file(TO_NATIVE_PATH ${DotPath} DotPath)
  endif(DOXYGEN_DOT_FOUND)

  # Request to configure the file
  configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)
  message("Doxygen build started")

  # Don't build by default
  add_custom_target(doxygen
    COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating documentation with Doxygen"
    VERBATIM)
  if(BUILD_DOC)
    add_custom_target(docs ALL DEPENDS doxygen)
  endif(BUILD_DOC)
elseif(DOXYGEN_FOUND)
  message("Doxygen needs to be installed to generate the doxygen documentation")
endif(DOXYGEN_FOUND)
