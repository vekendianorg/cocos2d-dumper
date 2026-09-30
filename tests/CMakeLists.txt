add_executable(stellar-tests
  main.cpp
  fixture_builder.cpp
  test_dwarf.cpp
  test_real_binary.cpp
  test_ir.cpp
  test_dwarfless.cpp
  test_normalize.cpp
  test_progress.cpp
)
target_link_libraries(stellar-tests PRIVATE stellar)
target_include_directories(stellar-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME stellar_unit_tests COMMAND stellar-tests)

# The real 583 MB target is exercised by a separate, explicitly opted-in test
# so the default `ctest` run stays fast.
add_test(NAME stellar_real_binary
         COMMAND stellar-tests --filter=RealBinary)
set_tests_properties(stellar_real_binary PROPERTIES TIMEOUT 900)
