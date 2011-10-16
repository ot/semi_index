cmake_minimum_required(VERSION 2.6)
project(SEMI_INDEX)

find_package(Boost 1.42.0 COMPONENTS unit_test_framework iostreams system thread filesystem REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})
link_directories (${Boost_LIBRARY_DIRS})

if (MSVC)
  # On Windows zlib is not in a standard location and has to be found
  find_package(zlib REQUIRED)
  include_directories(${ZLIB_INCLUDE_DIRS})
  link_directories (${ZLIB_LIBRARY_DIRS})
else(MSVC)
  set(ZLIB_LIBRARIES z)
endif(MSVC)

# For mongodb libraries -- windows builds need unicode
if(MSVC)
  add_definitions(-DUNICODE -D_UNICODE)
endif(MSVC)

if (UNIX)
  set(CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS} -g)
endif(UNIX)

set(CMAKE_BUILD_TYPE Release)
enable_testing()

# include 3rd-party dependencies
include_directories(3rd_party)
add_subdirectory(3rd_party)

# add the root directory to include path to make includes absolute
include_directories(${SEMI_INDEX_SOURCE_DIR})
# succinct subdirectory
add_subdirectory(succinct)
# semi_index subdirectory
add_subdirectory(semi_index)

# perftests
add_subdirectory(perftest)

# json_select (main executable)
add_executable(json_select json_select.cpp)
target_link_libraries(json_select
    succinct
    semi_index
    jsoncpp
    mongodblib
    ${Boost_LIBRARIES}
    )
  
