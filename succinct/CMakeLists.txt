set(SUCCINCT_SOURCES
  rs_bit_vector.cpp
  bp_vector.cpp
  )

add_library(succinct STATIC ${SUCCINCT_SOURCES})

file(GLOB SUCCINCT_TEST_SOURCES test_*.cpp)
foreach(TEST_SRC ${SUCCINCT_TEST_SOURCES})
  get_filename_component (TEST_SRC_NAME ${TEST_SRC} NAME_WE)
  add_executable(${TEST_SRC_NAME} ${TEST_SRC})
  target_link_libraries(${TEST_SRC_NAME}
    succinct
    ${Boost_LIBRARIES}
    )
  add_test(${TEST_SRC_NAME} ${TEST_SRC_NAME})
endforeach(TEST_SRC)

