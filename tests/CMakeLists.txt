add_executable(test_decoder test_decoder.c)
target_link_libraries(test_decoder PRIVATE cortex_m_core)
add_test(NAME decoder COMMAND test_decoder)

add_executable(test_executor test_executor.c)
target_link_libraries(test_executor PRIVATE cortex_m_core)
add_test(NAME executor COMMAND test_executor)

add_executable(test_bus test_bus.c)
target_link_libraries(test_bus PRIVATE cortex_m_core)
add_test(NAME bus COMMAND test_bus)

add_executable(test_memory test_memory.c)
target_link_libraries(test_memory PRIVATE cortex_m_core)
add_test(NAME memory COMMAND test_memory)
