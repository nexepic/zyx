function(zyx_normalize_source_list out_var)
    set(paths ${ARGN})
    list(REMOVE_DUPLICATES paths)
    list(SORT paths)
    set(${out_var} ${paths} PARENT_SCOPE)
endfunction()

function(zyx_filter_out_regex in_var regex)
    set(filtered)
    foreach(path IN LISTS ${in_var})
        if(NOT path MATCHES "${regex}")
            list(APPEND filtered "${path}")
        endif()
    endforeach()
    set(${in_var} ${filtered} PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE ZYX_INPUTXX_SOURCES_RAW CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/lib/inputxx/src/*.cpp"
)
zyx_normalize_source_list(ZYX_INPUTXX_SOURCES ${ZYX_INPUTXX_SOURCES_RAW})

set(ZYX_CYPHER_GENERATED_SOURCES
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/generated/CypherLexer.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/generated/CypherParser.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/generated/CypherParserBaseVisitor.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/generated/CypherParserVisitor.cpp"
)
set(ZYX_CYPHER_IMPLEMENTATION_SOURCES
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/CypherParserImpl.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/CypherToPlanVisitor.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/ir/CypherASTBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/helpers/public/OperatorChain.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/helpers/internal/AstExtractor.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/helpers/internal/ExpressionBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/helpers/internal/PatternBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/clauses/ReadingClauseHandler.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/clauses/WritingClauseHandler.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/clauses/AdminClauseHandler.cpp"
)
zyx_normalize_source_list(ZYX_CYPHER_SOURCES ${ZYX_CYPHER_GENERATED_SOURCES} ${ZYX_CYPHER_IMPLEMENTATION_SOURCES})

file(GLOB_RECURSE ZYX_CORE_SOURCES_RAW CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
)
set(ZYX_CORE_SOURCES ${ZYX_CORE_SOURCES_RAW})
zyx_filter_out_regex(ZYX_CORE_SOURCES "/src/query/parser/")
if(ZYX_WASM OR NOT ZYX_BUILD_APPS)
    zyx_filter_out_regex(ZYX_CORE_SOURCES "/src/cli/")
endif()
zyx_normalize_source_list(ZYX_CORE_SOURCES ${ZYX_CORE_SOURCES})

file(GLOB_RECURSE ZYX_TEST_SOURCES_RAW CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/tests/test_*.cpp"
    "${PROJECT_SOURCE_DIR}/tests/**/test_*.cpp"
    "${PROJECT_SOURCE_DIR}/lib/inputxx/tests/test_*.cpp"
    "${PROJECT_SOURCE_DIR}/lib/inputxx/tests/**/test_*.cpp"
)
zyx_normalize_source_list(ZYX_TEST_SOURCES ${ZYX_TEST_SOURCES_RAW})
