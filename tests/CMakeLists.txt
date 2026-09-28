add_executable(c2d-tests
  main.cpp
  fixture_builder.cpp
  test_dwarf.cpp
  test_real_binary.cpp
  test_ir.cpp
  test_dwarfless.cpp
)
target_link_libraries(c2d-tests PRIVATE c2d)
target_include_directories(c2d-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME c2d_unit_tests COMMAND c2d-tests)

# The real 583 MB target is exercised by a separate, explicitly opted-in test
# so the default `ctest` run stays fast.
add_test(NAME c2d_real_binary
         COMMAND c2d-tests --filter=RealBinary)
set_tests_properties(c2d_real_binary PROPERTIES TIMEOUT 900)
