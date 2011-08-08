set(SEMI_INDEX_SOURCES
  escape_table.cpp
  json_spirit_parser.cpp
  path_parser.cpp
  zrandom.cpp
  )

add_library(semi_index STATIC ${SEMI_INDEX_SOURCES})
target_link_libraries(semi_index ${ZLIB_LIBRARIES} ${Boost_LIBRARIES})

file(GLOB SEMI_INDEX_TEST_SOURCES test_*.cpp)
foreach(TEST_SRC ${SEMI_INDEX_TEST_SOURCES})
  get_filename_component (TEST_SRC_NAME ${TEST_SRC} NAME_WE)
  add_executable(${TEST_SRC_NAME} ${TEST_SRC})
  target_link_libraries(${TEST_SRC_NAME}
    succinct
    semi_index
    ${Boost_LIBRARIES}
    )
  add_test(${TEST_SRC_NAME} ${TEST_SRC_NAME})
endforeach(TEST_SRC)

