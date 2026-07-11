add_executable(CatalogJsonParserTest
  CatalogJsonParserTest.cpp
  ${REPO_ROOT}/lib/JsonParser/CatalogJsonParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(CatalogJsonParserTest PRIVATE
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(CatalogJsonParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogJsonParserTest)
