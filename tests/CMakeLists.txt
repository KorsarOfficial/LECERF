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

add_executable(test_t32 test_t32.c)
target_link_libraries(test_t32 PRIVATE cortex_m_core)
add_test(NAME t32 COMMAND test_t32)

add_executable(test_tt_determinism test_tt_determinism.c)
target_link_libraries(test_tt_determinism PRIVATE cortex_m_core)
add_test(NAME tt_determinism COMMAND test_tt_determinism)

add_executable(test_tt_snapshot test_tt_snapshot.c)
target_link_libraries(test_tt_snapshot PRIVATE cortex_m_core)
add_test(NAME tt_snapshot COMMAND test_tt_snapshot)

add_executable(test_tt_replay test_tt_replay.c)
target_link_libraries(test_tt_replay PRIVATE cortex_m_core)
add_test(NAME tt_replay COMMAND test_tt_replay)
