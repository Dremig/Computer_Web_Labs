add_library (spongechecks STATIC byte_stream_test_harness.cc
network_interface_test_harness.cc)

macro (add_test_exec exec_name)
    add_executable ("${exec_name}" "${exec_name}.cc")
    target_link_libraries ("${exec_name}" spongechecks ${ARGN})
    target_link_libraries ("${exec_name}" sponge ${ARGN})
endmacro (add_test_exec)

# lab3
add_test_exec (byte_stream_construction)
add_test_exec (byte_stream_one_write)
add_test_exec (byte_stream_two_writes)
add_test_exec (byte_stream_capacity)
add_test_exec (byte_stream_many_writes)

# lab4
add_test_exec (net_interface)