# Test suite — empty for scaffold.
#
# Once Phase 1 of the binding (see ../src/js.cc header) is in place,
# port the libjs test corpus over file by file:
#   - holepunchto/libjs/test/  is the source of truth for the
#     ABI's expected behavior.
#   - libqjs / libjsc / libmqjs each have their own copy of that
#     corpus, gated on the engine's feature support.
#
# For each test: copy the .c file into this directory, add the
# bare name to the `tests` list below, and the standard
# cmake-bare test harness will pick it up.

list(APPEND tests
  eval-script
  host-function
  global-host-binding
)

foreach(test IN LISTS tests)
  add_executable(${test} ${test}.c)
  target_link_libraries(${test} PRIVATE js_static)
  add_test(NAME ${test} COMMAND ${test})
endforeach()
