
add_subdirectory(sparqlParser)
add_subdirectory(data)

# GraphQL support (optional, enabled by default)
if (GRAPHQL_SUPPORT)
    add_subdirectory(graphql)
endif ()

add_library(parser
        sparqlParser/SparqlQleverVisitor.cpp
        SparqlParser.cpp
        ParsedQuery.cpp
        RdfParser.cpp
        Tokenizer.cpp
        WordsAndDocsFileParser.cpp
        ParallelBuffer.cpp
        SparqlParserHelpers.cpp
        TripleComponent.cpp
        GraphPatternOperation.cpp
        MagicServiceQuery.cpp
        NamedCachedResult.cpp
        PathQuery.cpp
        SpatialQuery.cpp
        PayloadVariables.cpp
        PropertyPath.cpp
        data/SparqlFilter.cpp
        SelectClause.cpp
        GraphPatternOperation.cpp
        GraphPattern.cpp
        LiteralOrIri.cpp
        DatasetClauses.cpp
        TextSearchQuery.cpp
        MaterializedViewQuery.cpp
        Quads.cpp
        UpdateTriples.cpp
        MaterializedViewQuery.cpp
)
qlever_target_link_libraries(parser sparqlParser parserData sparqlExpressions rdfEscaping global re2::re2 util engine index rdfTypes)

# Link GraphQL parser if enabled
if (GRAPHQL_SUPPORT)
    target_link_libraries(parser graphqlParser)
    target_compile_definitions(parser PUBLIC QLEVER_GRAPHQL_SUPPORT)
endif ()
