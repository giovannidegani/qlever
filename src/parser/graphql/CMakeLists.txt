# GraphQL Parser Integration for QLever
# NOTE: Currently using a simplified parser. Full cppgraphqlgen integration
# is pending due to API compatibility issues.

if (GRAPHQL_SUPPORT)
    add_library(graphqlParser
        GraphQLParser.cpp
        GraphQLToSparql.cpp
        GraphQLSchema.cpp
        GraphQLMutationTranslator.cpp
        MutationValidator.cpp
    )

    qlever_target_link_libraries(graphqlParser
        # Note: graphqlpeg dependency removed temporarily due to API issues
        # graphqlpeg      # cppgraphqlgen parsing library
        parserData
        util
    )

    # Include the cppgraphqlgen headers (if needed in the future)
    # target_include_directories(graphqlParser SYSTEM PUBLIC
    #     ${cppgraphqlgen_SOURCE_DIR}/include
    # )
endif ()
