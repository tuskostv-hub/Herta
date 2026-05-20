add_executable(test_lexer lexer/test_tokens.cpp)
target_link_libraries(test_lexer PRIVATE herta_core)
add_test(NAME lexer.tokens COMMAND test_lexer)

add_executable(test_parser parser/test_parser.cpp)
target_link_libraries(test_parser PRIVATE herta_core)
add_test(NAME parser.parse COMMAND test_parser)
