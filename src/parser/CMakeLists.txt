include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

bison_target(Parser parser.yy ${CMAKE_CURRENT_BINARY_DIR}/GraphParser.cpp COMPILE_FLAGS "-Werror")
flex_target(Scanner scanner.lex ${CMAKE_CURRENT_BINARY_DIR}/GraphScanner.cpp)

add_flex_bison_dependency(Scanner Parser)

add_compile_options(-Wno-error=sign-compare)

nebula_add_library(
    parser_obj OBJECT
    ${FLEX_Scanner_OUTPUTS}
    ${BISON_Parser_OUTPUTS}
    Clauses.cpp
    SequentialSentences.cpp
    MaintainSentences.cpp
    MutateSentences.cpp
    TraverseSentences.cpp
    AdminSentences.cpp
    UserSentences.cpp
)

add_subdirectory(test)
