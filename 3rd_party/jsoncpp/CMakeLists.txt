
include_directories(.)

file(GLOB JSONCPP_SOURCES *.cpp)
add_library(jsoncpp STATIC ${JSONCPP_SOURCES})

