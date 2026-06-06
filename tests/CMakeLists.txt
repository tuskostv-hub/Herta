add_executable(test_lexer lexer/test_tokens.cpp)
target_link_libraries(test_lexer PRIVATE herta_core)
add_test(NAME lexer.tokens COMMAND test_lexer)

add_executable(test_parser parser/test_parser.cpp)
target_link_libraries(test_parser PRIVATE herta_core)
add_test(NAME parser.parse COMMAND test_parser)

add_executable(test_semantic semantic/test_semantic.cpp)
target_link_libraries(test_semantic PRIVATE herta_core)
add_test(NAME semantic.check COMMAND test_semantic)

add_executable(test_modules driver/test_modules.cpp)
target_link_libraries(test_modules PRIVATE herta_core)
add_test(NAME driver.modules COMMAND test_modules)

add_executable(test_ir ir/test_ir.cpp)
target_link_libraries(test_ir PRIVATE herta_core)
add_test(NAME ir.lowering COMMAND test_ir)

# End-to-end интеграционный тест: компилирует каждый кейс через myc --run
# и проверяет stdout + код возврата.
add_executable(test_e2e e2e/test_e2e.cpp)
target_link_libraries(test_e2e PRIVATE herta_core)
target_compile_definitions(test_e2e PRIVATE MYC_PATH="$<TARGET_FILE:myc>")
add_dependencies(test_e2e myc)
add_test(NAME e2e.run COMMAND test_e2e)
