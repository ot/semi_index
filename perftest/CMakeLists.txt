
add_executable(json_semi_index_perftest json_semi_index_perftest.cpp)
target_link_libraries(json_semi_index_perftest
    succinct
    semi_index
    jsoncpp
    mongodblib
    ${Boost_LIBRARIES}
    )
  
