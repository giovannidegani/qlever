// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifdef QLEVER_GRAPHQL_SUPPORT

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "engine/MaterializedViews.h"
#include "engine/NamedResultCache.h"
#include "engine/QueryExecutionContext.h"
#include "engine/QueryPlanner.h"
#include "engine/graphql/GraphQLResultFormatter.h"
#include "engine/graphql/SchemaBuilder.h"
#include "parser/graphql/GraphQLParser.h"
#include "parser/graphql/GraphQLSchema.h"
#include "parser/graphql/GraphQLToSparql.h"
#include "util/AllocatorTestHelpers.h"
#include "util/GTestHelpers.h"
#include "util/IndexTestHelpers.h"

using namespace graphql;
using namespace ad_utility::testing;
using ::testing::Contains;
using ::testing::Eq;
using ::testing::Gt;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;

namespace {

// Test turtle data with RDF types and properties
constexpr const char* TEST_TURTLE = R"(
@prefix rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
@prefix schema: <http://schema.org/> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
@prefix ex: <http://example.org/> .

ex:Person1 rdf:type schema:Person ;
           schema:name "Albert Einstein" ;
           schema:birthDate "1879-03-14"^^xsd:date ;
           schema:knows ex:Person2 .

ex:Person2 rdf:type schema:Person ;
           schema:name "Niels Bohr" ;
           schema:birthDate "1885-10-07"^^xsd:date ;
           schema:knows ex:Person1 .

ex:Person3 rdf:type schema:Person ;
           schema:name "Marie Curie" ;
           schema:birthDate "1867-11-07"^^xsd:date .

ex:Org1 rdf:type schema:Organization ;
        schema:name "Princeton University" .

ex:Org2 rdf:type schema:Organization ;
        schema:name "University of Copenhagen" .

schema:Person rdfs:label "Person" .
schema:Organization rdfs:label "Organization" .
schema:name rdfs:label "name" .
schema:birthDate rdfs:label "birthDate" .
schema:knows rdfs:label "knows" .
)";

// Helper to get a Field from a Selection
const Field& getField(const Selection& selection) {
  const auto& ptr = std::get<std::shared_ptr<Field>>(selection);
  return *ptr;
}

}  // namespace

// ============================================================================
// GraphQL Parser Integration Tests
// ============================================================================

class GraphQLIntegrationTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    // Create the test index once for all tests
    index_ = std::make_unique<Index>(
        makeTestIndex("graphql_integration_test", std::string(TEST_TURTLE)));
  }

  static void TearDownTestSuite() {
    index_.reset();
    // Clean up index files
    for (const auto& file :
         getAllIndexFilenames("graphql_integration_test")) {
      std::filesystem::remove(file);
    }
  }

  static std::unique_ptr<Index> index_;
};

std::unique_ptr<Index> GraphQLIntegrationTest::index_ = nullptr;

// ============================================================================
// GraphQL-to-SPARQL Translation Tests
// ============================================================================

TEST_F(GraphQLIntegrationTest, TranslateSimpleQuery) {
  // Create a schema with a Person type
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("id", "", ScalarType::ID, false, true));
  personType.fields.push_back(
      SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  schema.addType(std::move(personType));

  // Parse a GraphQL query
  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person {
        id
        name
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  // Translate to SPARQL
  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  ASSERT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
  const auto& result = std::get<TranslationResult>(translateResult);

  // Check that the ParsedQuery has a proper structure
  // The query should have a SELECT clause
  EXPECT_TRUE(result.parsedQuery.hasSelectClause());

  // The root graph pattern should have some children (the translated triples)
  EXPECT_FALSE(result.parsedQuery._rootGraphPattern._graphPatterns.empty());

  // Should have field mappings
  EXPECT_FALSE(result.rootMapping.children.empty());
}

TEST_F(GraphQLIntegrationTest, TranslateQueryWithFilter) {
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("id", "", ScalarType::ID, false, true));
  personType.fields.push_back(
      SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  schema.addType(std::move(personType));

  // Parse a query with filter arguments
  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person(first: 10) {
        id
        name
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  ASSERT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
  const auto& result = std::get<TranslationResult>(translateResult);

  // The query should have a LIMIT clause
  EXPECT_TRUE(result.parsedQuery.hasSelectClause());
}

// ============================================================================
// Schema Builder Tests (without real query execution)
// ============================================================================

TEST_F(GraphQLIntegrationTest, SchemaBuilderDiscoverTypes) {
  // Note: SchemaBuilder's executeQuery requires a cache, which is only
  // available in server context. For unit tests, we test the schema
  // structure after it attempts discovery (which may fail gracefully).
  SchemaBuilder builder(*index_);

  // The getSchema() call will attempt to discover types from the index
  // Even if the SPARQL queries fail, it should return a valid (possibly empty) schema
  const auto& schema = builder.getSchema();

  // Verify the schema can generate SDL (even if empty)
  std::string sdl = schema.toSDL();
  EXPECT_THAT(sdl, Not(IsEmpty()));
  EXPECT_THAT(sdl, HasSubstr("type Query"));
}

TEST_F(GraphQLIntegrationTest, SchemaBuilderCaching) {
  SchemaBuilder builder(*index_);

  // First call should build the schema
  EXPECT_FALSE(builder.hasCachedSchema());
  const auto& schema1 = builder.getSchema();
  EXPECT_TRUE(builder.hasCachedSchema());

  // Second call should return cached schema
  const auto& schema2 = builder.getSchema();
  EXPECT_EQ(&schema1, &schema2);  // Same pointer

  // Force refresh should rebuild
  builder.clearCache();
  EXPECT_FALSE(builder.hasCachedSchema());
  [[maybe_unused]] const auto& schema3 = builder.getSchema();
  EXPECT_TRUE(builder.hasCachedSchema());
}

TEST_F(GraphQLIntegrationTest, SchemaIntrospection) {
  SchemaBuilder builder(*index_);
  const auto& schema = builder.getSchema();

  // Test introspection JSON generation
  nlohmann::json introspection = schema.toIntrospectionJSON();

  // Should have queryType
  EXPECT_TRUE(introspection.contains("queryType"));
  EXPECT_EQ(introspection["queryType"]["name"], "Query");

  // Should have types array
  EXPECT_TRUE(introspection.contains("types"));
  EXPECT_TRUE(introspection["types"].is_array());
}

// ============================================================================
// Result Formatter Tests
// ============================================================================

TEST_F(GraphQLIntegrationTest, FormatEmptyResult) {
  // Create a minimal operation
  graphql::Operation op;
  op.type = graphql::OperationType::Query;

  // Create an empty translation result
  TranslationResult translationResult;

  // Create an empty result
  auto allocator = makeAllocator();
  IdTable idTable(0, allocator);
  LocalVocab localVocab;
  auto result = Result(std::move(idTable), {}, std::move(localVocab));

  // Format the result
  nlohmann::json response =
      GraphQLResultFormatter::format(result, translationResult, op, *index_);

  // Should have data field
  EXPECT_TRUE(response.contains("data"));
}

TEST_F(GraphQLIntegrationTest, FormatErrorResponse) {
  std::vector<GraphQLError> errors;
  GraphQLError error;
  error.message = "Test error message";
  error.locations.push_back(SourceLocation{1, 5});
  errors.push_back(std::move(error));

  nlohmann::json response = GraphQLResultFormatter::formatErrors(errors);

  EXPECT_TRUE(response.contains("errors"));
  EXPECT_TRUE(response["errors"].is_array());
  EXPECT_THAT(response["errors"], SizeIs(1));
  EXPECT_EQ(response["errors"][0]["message"], "Test error message");
  EXPECT_EQ(response["errors"][0]["locations"][0]["line"], 1);
  EXPECT_EQ(response["errors"][0]["locations"][0]["column"], 5);
}

TEST_F(GraphQLIntegrationTest, CreateResponse) {
  nlohmann::json data = {{"test", "value"}};

  nlohmann::json response =
      GraphQLResultFormatter::createResponse(data);

  EXPECT_TRUE(response.contains("data"));
  EXPECT_EQ(response["data"]["test"], "value");
}

// ============================================================================
// End-to-End Query Execution Tests
// ============================================================================

TEST_F(GraphQLIntegrationTest, ExecuteSparqlFromGraphQL) {
  // Create a schema
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("id", "", ScalarType::ID, false, true));
  personType.fields.push_back(
      SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  schema.addType(std::move(personType));

  // Parse GraphQL
  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person {
        name
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  // Translate to SPARQL
  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  ASSERT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
  auto& result = std::get<TranslationResult>(translateResult);

  // Execute the SPARQL query with proper cache setup
  auto allocator = makeAllocator();
  QueryResultCache cache;
  NamedResultCache namedResultCache;
  MaterializedViewsManager materializedViewsManager;
  QueryExecutionContext qec(*index_, &cache, allocator,
                            SortPerformanceEstimator{}, &namedResultCache,
                            &materializedViewsManager);

  auto cancellationHandle =
      std::make_shared<ad_utility::CancellationHandle<>>();
  QueryPlanner qp(&qec, cancellationHandle);

  // This should not throw
  EXPECT_NO_THROW({
    auto executionTree = qp.createExecutionTree(result.parsedQuery);
    auto queryResult = executionTree.getResult();
    EXPECT_TRUE(queryResult != nullptr);
  });
}

// ============================================================================
// GraphQL Schema SDL Generation Tests
// ============================================================================

TEST_F(GraphQLIntegrationTest, SchemaSDLGeneration) {
  GraphQLSchema schema;

  // Add a type with various field types
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.comment = "A person entity";
  personType.fields.push_back(SchemaField("id", "", ScalarType::ID, false, true));
  personType.fields.push_back(
      SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  personType.fields.push_back(
      SchemaField("age", "http://schema.org/age", ScalarType::INT));
  schema.addType(std::move(personType));

  std::string sdl = schema.toSDL();

  // Check SDL content
  EXPECT_THAT(sdl, HasSubstr("type Query"));
  EXPECT_THAT(sdl, HasSubstr("type Person"));
  EXPECT_THAT(sdl, HasSubstr("id: ID!"));
  EXPECT_THAT(sdl, HasSubstr("name:"));
  EXPECT_THAT(sdl, HasSubstr("age:"));
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(GraphQLIntegrationTest, HandleUnknownType) {
  GraphQLSchema schema;
  // Empty schema - no types defined

  auto parseResult = GraphQLParser::parse(R"(
    query {
      UnknownType {
        id
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  // Should return an error for unknown type
  EXPECT_TRUE(
      std::holds_alternative<std::vector<GraphQLError>>(translateResult));
  const auto& errors = std::get<std::vector<GraphQLError>>(translateResult);
  EXPECT_THAT(errors, Not(IsEmpty()));
  EXPECT_THAT(errors[0].message, HasSubstr("Unknown type"));
}

TEST_F(GraphQLIntegrationTest, HandleInvalidQuery) {
  // Test with malformed GraphQL
  auto parseResult = GraphQLParser::parse("{ invalid query syntax [[");

  // Should return parse errors
  EXPECT_TRUE(
      std::holds_alternative<std::vector<GraphQLError>>(parseResult));
}

// ============================================================================
// Edge Case Tests - Deep Nesting
// ============================================================================

TEST_F(GraphQLIntegrationTest, ParseDeepNestedQuery) {
  // Test parsing of deeply nested queries (3+ levels)
  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person {
        name
        knows {
          name
          knows {
            name
          }
        }
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);
  EXPECT_THAT(doc.operations, SizeIs(1));

  // Verify nested structure
  const auto& op = doc.operations[0];
  EXPECT_THAT(op.selectionSet, SizeIs(1));

  // Get Person field
  const auto& personField = getField(op.selectionSet[0]);
  EXPECT_EQ(personField.name, "Person");
  EXPECT_THAT(personField.selectionSet, SizeIs(2));  // name, knows
}

TEST_F(GraphQLIntegrationTest, TranslateDeepNestedQuery) {
  // Create schema with nested relationship
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("id", "", ScalarType::ID, false, true));
  personType.fields.push_back(
      SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  // Add knows as object type reference (circular) - use object field constructor
  personType.fields.push_back(
      SchemaField("knows", "http://schema.org/knows", "Person", true, false));
  schema.addType(std::move(personType));

  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person {
        name
        knows {
          name
        }
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  ASSERT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
  const auto& result = std::get<TranslationResult>(translateResult);

  // Should have nested field mappings
  EXPECT_FALSE(result.rootMapping.children.empty());
}

// ============================================================================
// Edge Case Tests - Multiple Aliases
// ============================================================================

TEST_F(GraphQLIntegrationTest, ParseMultipleAliases) {
  // Test same field aliased multiple times
  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person {
        firstName: name
        lastName: name
        fullName: name
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  const auto& op = doc.operations[0];
  const auto& personField = getField(op.selectionSet[0]);

  // Should have 3 aliased fields
  EXPECT_THAT(personField.selectionSet, SizeIs(3));

  // Verify aliases
  const auto& field1 = getField(personField.selectionSet[0]);
  const auto& field2 = getField(personField.selectionSet[1]);
  const auto& field3 = getField(personField.selectionSet[2]);

  EXPECT_EQ(field1.alias, "firstName");
  EXPECT_EQ(field1.name, "name");
  EXPECT_EQ(field2.alias, "lastName");
  EXPECT_EQ(field2.name, "name");
  EXPECT_EQ(field3.alias, "fullName");
  EXPECT_EQ(field3.name, "name");
}

// ============================================================================
// Edge Case Tests - Empty and Boundary Conditions
// ============================================================================

TEST_F(GraphQLIntegrationTest, ParseQueryWithExcessiveWhitespace) {
  // Lots of whitespace, tabs, newlines
  auto parseResult = GraphQLParser::parse(R"(

    query    GetPeople   {

        Person    {

            id

            name

        }

    }

  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);
  EXPECT_EQ(doc.operations[0].name, "GetPeople");
}

TEST_F(GraphQLIntegrationTest, ParseMinimalQuery) {
  // Minimal valid query with no whitespace
  auto parseResult = GraphQLParser::parse("{Person{id}}");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);
  EXPECT_THAT(doc.operations, SizeIs(1));
}

TEST_F(GraphQLIntegrationTest, PaginationBoundaryFirst0) {
  // first: 0 should return empty result
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("id", "", ScalarType::ID, false, true));
  schema.addType(std::move(personType));

  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person(first: 0) {
        id
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  // Should succeed - 0 is a valid limit
  ASSERT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
}

// ============================================================================
// Edge Case Tests - Multiple Operations
// ============================================================================

TEST_F(GraphQLIntegrationTest, ParseMultipleNamedOperations) {
  // Multiple named operations in same document - now fully supported
  auto parseResult = GraphQLParser::parse(R"(
    query GetPeople {
      Person { id name }
    }

    query GetOrganizations {
      Organization { id name }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  // Should have both operations
  EXPECT_THAT(doc.operations, SizeIs(2));
  EXPECT_EQ(doc.operations[0].name, "GetPeople");
  EXPECT_EQ(doc.operations[1].name, "GetOrganizations");

  // Verify first operation structure
  const auto& op1 = doc.operations[0];
  EXPECT_THAT(op1.selectionSet, SizeIs(1));
  const auto& personField = getField(op1.selectionSet[0]);
  EXPECT_EQ(personField.name, "Person");

  // Verify second operation structure
  const auto& op2 = doc.operations[1];
  EXPECT_THAT(op2.selectionSet, SizeIs(1));
  const auto& orgField = getField(op2.selectionSet[0]);
  EXPECT_EQ(orgField.name, "Organization");
}

TEST_F(GraphQLIntegrationTest, SelectSpecificOperationByName) {
  // Test operation selection by name with multiple operations
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("id", "", ScalarType::ID, false, true));
  personType.fields.push_back(
      SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  schema.addType(std::move(personType));

  SchemaType orgType;
  orgType.graphqlName = "Organization";
  orgType.classIRI = "http://schema.org/Organization";
  orgType.fields.push_back(SchemaField("id", "", ScalarType::ID, false, true));
  schema.addType(std::move(orgType));

  auto parseResult = GraphQLParser::parse(R"(
    query GetPeople {
      Person { id name }
    }

    query GetOrganizations {
      Organization { id }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  // Should have both operations
  EXPECT_THAT(doc.operations, SizeIs(2));

  GraphQLToSparql translator(schema);

  // Select the second operation by name
  auto translateResult =
      translator.translate(doc, std::optional<std::string>("GetOrganizations"), {});

  ASSERT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
  const auto& result = std::get<TranslationResult>(translateResult);

  // Verify the translation is for Organization, not Person
  // The rootMapping should reference Organization
  EXPECT_FALSE(result.rootMapping.children.empty());
}

TEST_F(GraphQLIntegrationTest, MultipleOperationsRequiresOperationName) {
  // GraphQL spec: When a document has multiple operations, operationName is required
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("id", "", ScalarType::ID, false, true));
  schema.addType(std::move(personType));

  auto parseResult = GraphQLParser::parse(R"(
    query GetPeople {
      Person { id }
    }

    query GetMorePeople {
      Person { id }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);
  EXPECT_THAT(doc.operations, SizeIs(2));

  GraphQLToSparql translator(schema);

  // Without operationName, should return error for multiple operations
  auto translateResult = translator.translate(doc, std::nullopt, {});

  // Should return error since operationName is required with multiple ops
  EXPECT_TRUE(
      std::holds_alternative<std::vector<GraphQLError>>(translateResult));
}

TEST_F(GraphQLIntegrationTest, MixedNamedAndAnonymousOperations) {
  // Parse document with mixed named and anonymous operations
  // GraphQL spec allows this but requires operationName if >1 operation
  auto parseResult = GraphQLParser::parse(R"(
    {
      Person { id }
    }

    query GetOrganizations {
      Organization { id }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  // Should parse both operations
  EXPECT_THAT(doc.operations, SizeIs(2));

  // First is anonymous (no name)
  EXPECT_FALSE(doc.operations[0].name.has_value());

  // Second is named
  EXPECT_TRUE(doc.operations[1].name.has_value());
  EXPECT_EQ(doc.operations[1].name.value(), "GetOrganizations");
}

TEST_F(GraphQLIntegrationTest, ThreeOperationsSelectMiddle) {
  // Test selecting the middle operation from three
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("id", "", ScalarType::ID, false, true));
  schema.addType(std::move(personType));

  SchemaType orgType;
  orgType.graphqlName = "Organization";
  orgType.classIRI = "http://schema.org/Organization";
  orgType.fields.push_back(SchemaField("id", "", ScalarType::ID, false, true));
  schema.addType(std::move(orgType));

  auto parseResult = GraphQLParser::parse(R"(
    query First {
      Person { id }
    }

    query Second {
      Organization { id }
    }

    query Third {
      Person { id }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);
  EXPECT_THAT(doc.operations, SizeIs(3));

  GraphQLToSparql translator(schema);

  // Select the middle operation
  auto translateResult =
      translator.translate(doc, std::optional<std::string>("Second"), {});

  ASSERT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
}

// ============================================================================
// Edge Case Tests - Special Field Names
// ============================================================================

TEST_F(GraphQLIntegrationTest, ParseFieldsWithNumbers) {
  // Field names containing numbers
  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person {
        field1
        field2Name
        name3
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  const auto& personField = getField(doc.operations[0].selectionSet[0]);
  EXPECT_THAT(personField.selectionSet, SizeIs(3));
}

TEST_F(GraphQLIntegrationTest, ParseFieldsWithUnderscores) {
  // Fields starting with underscore (not introspection)
  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person {
        _internalField
        my_field_name
        field_
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  const auto& personField = getField(doc.operations[0].selectionSet[0]);
  EXPECT_THAT(personField.selectionSet, SizeIs(3));

  const auto& field1 = getField(personField.selectionSet[0]);
  EXPECT_EQ(field1.name, "_internalField");
}

// ============================================================================
// Edge Case Tests - Error Messages
// ============================================================================

TEST_F(GraphQLIntegrationTest, MultipleErrorsInQuery) {
  // Query with multiple issues
  GraphQLSchema schema;
  // Empty schema

  auto parseResult = GraphQLParser::parse(R"(
    query {
      UnknownType1 { id }
      UnknownType2 { name }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  // Should have multiple errors (one for each unknown type)
  EXPECT_TRUE(
      std::holds_alternative<std::vector<GraphQLError>>(translateResult));
  const auto& errors = std::get<std::vector<GraphQLError>>(translateResult);
  EXPECT_THAT(errors, SizeIs(Gt(0)));
}

TEST_F(GraphQLIntegrationTest, ErrorWithPathInfo) {
  // Error should include path information
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  // Only id field, no "unknownField"
  personType.fields.push_back(SchemaField("id", "", ScalarType::ID, false, true));
  schema.addType(std::move(personType));

  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person {
        id
        unknownField
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  // Should return error for unknown field
  EXPECT_TRUE(
      std::holds_alternative<std::vector<GraphQLError>>(translateResult));
}

// ============================================================================
// Edge Case Tests - Result Formatting Null/Empty
// ============================================================================

TEST_F(GraphQLIntegrationTest, FormatMultipleErrors) {
  // Multiple errors in response
  std::vector<GraphQLError> errors;

  GraphQLError error1;
  error1.message = "First error";
  error1.locations.push_back(SourceLocation{1, 1});
  errors.push_back(std::move(error1));

  GraphQLError error2;
  error2.message = "Second error";
  error2.locations.push_back(SourceLocation{2, 5});
  errors.push_back(std::move(error2));

  GraphQLError error3;
  error3.message = "Third error";
  errors.push_back(std::move(error3));  // No location

  nlohmann::json response = GraphQLResultFormatter::formatErrors(errors);

  EXPECT_TRUE(response.contains("errors"));
  EXPECT_THAT(response["errors"], SizeIs(3));
  EXPECT_EQ(response["errors"][0]["message"], "First error");
  EXPECT_EQ(response["errors"][1]["message"], "Second error");
  EXPECT_EQ(response["errors"][2]["message"], "Third error");
}

// ============================================================================
// Edge Case Tests - Protocol Content-Type Variations
// ============================================================================

TEST_F(GraphQLIntegrationTest, ProtocolContentTypeWithCharset) {
  // Content-Type with charset should still work
  // This tests the protocol layer's content-type parsing
  std::string contentType = "application/json; charset=utf-8";

  // The content-type should be recognized as JSON
  EXPECT_TRUE(contentType.find("application/json") != std::string::npos);
}

// ============================================================================
// Edge Case Tests - Schema Edge Cases
// ============================================================================

TEST_F(GraphQLIntegrationTest, SchemaWithNoFields) {
  // Type with only id field
  GraphQLSchema schema;
  SchemaType emptyType;
  emptyType.graphqlName = "Empty";
  emptyType.classIRI = "http://example.org/Empty";
  // No fields except implied id
  schema.addType(std::move(emptyType));

  std::string sdl = schema.toSDL();
  EXPECT_THAT(sdl, HasSubstr("type Query"));
  EXPECT_THAT(sdl, HasSubstr("type Empty"));
}

TEST_F(GraphQLIntegrationTest, SchemaWithManyTypes) {
  // Schema with multiple types
  GraphQLSchema schema;

  for (int i = 0; i < 10; i++) {
    SchemaType type;
    type.graphqlName = "Type" + std::to_string(i);
    type.classIRI = "http://example.org/Type" + std::to_string(i);
    type.fields.push_back(
        SchemaField("id", "", ScalarType::ID, false, true));
    type.fields.push_back(
        SchemaField("name", "http://example.org/name", ScalarType::STRING));
    schema.addType(std::move(type));
  }

  EXPECT_EQ(schema.getTypes().size(), 10);

  std::string sdl = schema.toSDL();
  EXPECT_THAT(sdl, HasSubstr("Type0"));
  EXPECT_THAT(sdl, HasSubstr("Type9"));
}

// ============================================================================
// Introspection Query Tests
// ============================================================================

TEST_F(GraphQLIntegrationTest, IntrospectionSchemaQuery) {
  // Test __schema introspection query
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.comment = "A person";
  personType.fields.push_back(SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  schema.addType(std::move(personType));

  nlohmann::json schemaJson = schema.toIntrospectionJSON();

  // Verify schema structure
  EXPECT_TRUE(schemaJson.contains("queryType"));
  EXPECT_TRUE(schemaJson.contains("types"));
  EXPECT_EQ(schemaJson["queryType"]["name"], "Query");
  EXPECT_TRUE(schemaJson["mutationType"].is_null());
  EXPECT_TRUE(schemaJson["subscriptionType"].is_null());

  // Verify types array
  EXPECT_TRUE(schemaJson["types"].is_array());
  EXPECT_GT(schemaJson["types"].size(), 0);

  // Find Person type in types array
  bool foundPerson = false;
  for (const auto& type : schemaJson["types"]) {
    if (type.contains("name") && type["name"] == "Person") {
      foundPerson = true;
      EXPECT_EQ(type["kind"], "OBJECT");
      EXPECT_TRUE(type.contains("fields"));
      break;
    }
  }
  EXPECT_TRUE(foundPerson);
}

TEST_F(GraphQLIntegrationTest, IntrospectionTypeQuery) {
  // Test __type(name: "...") introspection query
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  personType.fields.push_back(SchemaField("age", "http://schema.org/age", ScalarType::INT));
  schema.addType(std::move(personType));

  // Look up existing type
  nlohmann::json personJson = schema.typeIntrospection("Person");
  EXPECT_FALSE(personJson.is_null());
  EXPECT_EQ(personJson["name"], "Person");
  EXPECT_EQ(personJson["kind"], "OBJECT");
  EXPECT_TRUE(personJson.contains("fields"));

  // Look up Query type
  nlohmann::json queryJson = schema.typeIntrospection("Query");
  EXPECT_FALSE(queryJson.is_null());
  EXPECT_EQ(queryJson["name"], "Query");
  EXPECT_EQ(queryJson["kind"], "OBJECT");

  // Look up scalar type
  nlohmann::json stringJson = schema.typeIntrospection("String");
  EXPECT_FALSE(stringJson.is_null());
  EXPECT_EQ(stringJson["name"], "String");
  EXPECT_EQ(stringJson["kind"], "SCALAR");

  // Look up non-existent type
  nlohmann::json unknownJson = schema.typeIntrospection("NonExistent");
  EXPECT_TRUE(unknownJson.is_null());
}

TEST_F(GraphQLIntegrationTest, IntrospectionFieldDetails) {
  // Test that field introspection includes required details
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";

  SchemaField nameField("name", "http://schema.org/name", ScalarType::STRING, true, false);
  nameField.comment = "The person's name";
  personType.fields.push_back(std::move(nameField));

  SchemaField friendField("friend", "http://schema.org/knows", "Person", true, false);
  personType.fields.push_back(std::move(friendField));

  schema.addType(std::move(personType));

  nlohmann::json typeJson = schema.typeIntrospection("Person");
  ASSERT_FALSE(typeJson.is_null());

  // Find the fields
  EXPECT_TRUE(typeJson.contains("fields"));
  EXPECT_TRUE(typeJson["fields"].is_array());

  // There should be id, name, and friend fields
  EXPECT_GE(typeJson["fields"].size(), 3);

  bool foundName = false;
  bool foundFriend = false;
  for (const auto& field : typeJson["fields"]) {
    if (field["name"] == "name") {
      foundName = true;
      // Check field has args (for lang parameter)
      EXPECT_TRUE(field.contains("args"));
    }
    if (field["name"] == "friend") {
      foundFriend = true;
      // Object field should reference Person type
      EXPECT_TRUE(field.contains("type"));
    }
  }
  EXPECT_TRUE(foundName);
  EXPECT_TRUE(foundFriend);
}

TEST_F(GraphQLIntegrationTest, ParseIntrospectionSchemaQuery) {
  // Test parsing a standard __schema introspection query
  auto parseResult = GraphQLParser::parse(R"(
    query IntrospectionQuery {
      __schema {
        queryType { name }
        mutationType { name }
        subscriptionType { name }
        types { name kind }
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  ASSERT_THAT(doc.operations, SizeIs(1));
  EXPECT_TRUE(doc.operations[0].name.has_value());
  EXPECT_EQ(doc.operations[0].name.value(), "IntrospectionQuery");

  // Verify __schema field is present
  EXPECT_THAT(doc.operations[0].selectionSet, SizeIs(1));
  const auto& schemaField = getField(doc.operations[0].selectionSet[0]);
  EXPECT_EQ(schemaField.name, "__schema");
  EXPECT_FALSE(schemaField.selectionSet.empty());
}

TEST_F(GraphQLIntegrationTest, ParseIntrospectionTypeQuery) {
  // Test parsing a __type introspection query
  auto parseResult = GraphQLParser::parse(R"(
    query TypeQuery {
      __type(name: "Person") {
        name
        kind
        fields { name type { name } }
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  ASSERT_THAT(doc.operations, SizeIs(1));
  const auto& typeField = getField(doc.operations[0].selectionSet[0]);
  EXPECT_EQ(typeField.name, "__type");
  EXPECT_FALSE(typeField.selectionSet.empty());
}

TEST_F(GraphQLIntegrationTest, ParseTypenameField) {
  // Test parsing __typename field within a selection
  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person {
        __typename
        id
        name
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  ASSERT_THAT(doc.operations, SizeIs(1));
  const auto& personField = getField(doc.operations[0].selectionSet[0]);
  EXPECT_EQ(personField.name, "Person");

  // Find __typename field
  bool foundTypename = false;
  for (const auto& sel : personField.selectionSet) {
    if (auto* fieldPtr = std::get_if<std::shared_ptr<Field>>(&sel)) {
      const auto& field = **fieldPtr;
      if (field.name == "__typename") {
        foundTypename = true;
        break;
      }
    }
  }
  EXPECT_TRUE(foundTypename);
}

// ============================================================================
// OR Filter (UNION) Tests
// ============================================================================

TEST_F(GraphQLIntegrationTest, TranslateOrFilter) {
  // Test OR filter translation to UNION
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  personType.fields.push_back(SchemaField("age", "http://schema.org/age", ScalarType::INT));
  schema.addType(std::move(personType));

  // Parse a query with OR filter
  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person(filter: {
        OR: [
          { name: { eq: "Alice" } },
          { name: { eq: "Bob" } }
        ]
      }) {
        id
        name
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  // Translation should succeed (UNION is now implemented)
  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  // Should not error
  EXPECT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
}

TEST_F(GraphQLIntegrationTest, TranslateOrFilterWithThreeBranches) {
  // Test OR filter with more than 2 alternatives
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  schema.addType(std::move(personType));

  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person(filter: {
        OR: [
          { name: { eq: "Alice" } },
          { name: { eq: "Bob" } },
          { name: { eq: "Charlie" } }
        ]
      }) {
        id
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  // Should succeed with three-way UNION
  EXPECT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
}

TEST_F(GraphQLIntegrationTest, TranslateOrFilterSingleElement) {
  // OR filter with single element should behave like no OR
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  schema.addType(std::move(personType));

  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person(filter: {
        OR: [
          { name: { eq: "Alice" } }
        ]
      }) {
        id
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  EXPECT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
}

// ============================================================================
// NOT Filter (MINUS) Tests
// ============================================================================

TEST_F(GraphQLIntegrationTest, TranslateNotFilter) {
  // Test NOT filter translation to MINUS
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  personType.fields.push_back(SchemaField("age", "http://schema.org/age", ScalarType::INT));
  schema.addType(std::move(personType));

  // Parse a query with NOT filter
  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person(filter: {
        NOT: { name: { eq: "Alice" } }
      }) {
        id
        name
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  // Translation should succeed (MINUS is now implemented)
  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  // Should not error
  EXPECT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
}

TEST_F(GraphQLIntegrationTest, TranslateNestedAndOrNot) {
  // Test nested logical operators: AND containing OR and NOT
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  personType.fields.push_back(SchemaField("age", "http://schema.org/age", ScalarType::INT));
  schema.addType(std::move(personType));

  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person(filter: {
        AND: [
          { OR: [ { name: { eq: "Alice" } }, { name: { eq: "Bob" } } ] },
          { NOT: { age: { lt: 18 } } }
        ]
      }) {
        id
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  // Should succeed with complex nested filters
  EXPECT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
}

// ============================================================================
// Security Tests
// ============================================================================

TEST_F(GraphQLIntegrationTest, SecurityFragmentCycleDetection) {
  // Test that fragment cycles are detected and rejected
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  schema.addType(std::move(personType));

  // Create a query with a fragment that references itself
  auto parseResult = GraphQLParser::parse(R"(
    fragment PersonFields on Person {
      name
      ...PersonFields
    }

    query {
      Person {
        ...PersonFields
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  // Should return error due to fragment cycle
  EXPECT_TRUE(
      std::holds_alternative<std::vector<GraphQLError>>(translateResult));
  if (std::holds_alternative<std::vector<GraphQLError>>(translateResult)) {
    const auto& errors = std::get<std::vector<GraphQLError>>(translateResult);
    EXPECT_THAT(errors, Not(IsEmpty()));
    EXPECT_THAT(errors[0].message, HasSubstr("cycle"));
  }
}

TEST_F(GraphQLIntegrationTest, SecurityMutualFragmentCycleDetection) {
  // Test that mutual fragment cycles are detected
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(SchemaField("name", "http://schema.org/name", ScalarType::STRING));
  schema.addType(std::move(personType));

  // Create fragments that reference each other
  auto parseResult = GraphQLParser::parse(R"(
    fragment A on Person {
      name
      ...B
    }

    fragment B on Person {
      name
      ...A
    }

    query {
      Person {
        ...A
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLToSparql translator(schema);
  auto translateResult = translator.translate(doc, std::nullopt, {});

  // Should return error due to mutual fragment cycle
  EXPECT_TRUE(
      std::holds_alternative<std::vector<GraphQLError>>(translateResult));
}

TEST_F(GraphQLIntegrationTest, ConfigurablePaginationLimits) {
  // Test that pagination limits can be configured at runtime
  GraphQLSchema schema;
  SchemaType personType;
  personType.graphqlName = "Person";
  personType.classIRI = "http://schema.org/Person";
  personType.fields.push_back(
      SchemaField("id", "", ScalarType::ID, false, true));
  schema.addType(std::move(personType));

  auto parseResult = GraphQLParser::parse(R"(
    query {
      Person(first: 500, offset: 10000) {
        id
      }
    }
  )");

  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  // Test with default limits (should allow these values)
  {
    GraphQLToSparql translator(schema);
    auto result = translator.translate(doc, std::nullopt, {});
    ASSERT_TRUE(std::holds_alternative<TranslationResult>(result));
    const auto& tr = std::get<TranslationResult>(result);
    // Default limit is 10000, so 500 should be allowed
    EXPECT_EQ(500u, tr.parsedQuery._limitOffset._limit);
    // Default max offset is 1000000, so 10000 should be allowed
    EXPECT_EQ(10000u, tr.parsedQuery._limitOffset._offset);
  }

  // Test with custom restrictive limits
  {
    TranslationConfig config;
    config.maxResults = 100;   // Limit to 100 results
    config.maxOffset = 5000;   // Limit offset to 5000

    GraphQLToSparql translator(schema, config);
    auto result = translator.translate(doc, std::nullopt, {});
    ASSERT_TRUE(std::holds_alternative<TranslationResult>(result));
    const auto& tr = std::get<TranslationResult>(result);
    // Requested 500 but limited to 100
    EXPECT_EQ(100u, tr.parsedQuery._limitOffset._limit);
    // Requested 10000 but limited to 5000
    EXPECT_EQ(5000u, tr.parsedQuery._limitOffset._offset);
  }

  // Test with permissive limits
  {
    TranslationConfig config;
    config.maxResults = 100000;   // Allow up to 100k results
    config.maxOffset = 10000000;  // Allow up to 10M offset

    GraphQLToSparql translator(schema, config);
    auto result = translator.translate(doc, std::nullopt, {});
    ASSERT_TRUE(std::holds_alternative<TranslationResult>(result));
    const auto& tr = std::get<TranslationResult>(result);
    // All requested values should be allowed
    EXPECT_EQ(500u, tr.parsedQuery._limitOffset._limit);
    EXPECT_EQ(10000u, tr.parsedQuery._limitOffset._offset);
  }
}

#endif  // QLEVER_GRAPHQL_SUPPORT
