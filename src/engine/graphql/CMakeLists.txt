# GraphQL support library for QLever engine
# Only built when GRAPHQL_SUPPORT is enabled

if (GRAPHQL_SUPPORT)
    add_library(graphqlEngine
        GraphQLProtocol.cpp
        GraphQLResultFormatter.cpp
        SchemaBuilder.cpp
        OwlViewGenerator.cpp
        MutationSchemaBuilder.cpp
    )

    qlever_target_link_libraries(graphqlEngine
        graphqlParser
        engine
        util
        http
    )

    target_include_directories(graphqlEngine PUBLIC
        ${CMAKE_SOURCE_DIR}/src
    )
endif()
