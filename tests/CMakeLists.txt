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
