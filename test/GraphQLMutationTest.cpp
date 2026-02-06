// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#include <gtest/gtest.h>

#include "parser/graphql/GraphQLMutationTranslator.h"
#include "parser/graphql/GraphQLParser.h"
#include "parser/graphql/MutationValidator.h"
#include "engine/graphql/MutationSchemaBuilder.h"

using namespace graphql;

// ============================================================================
// Test Fixtures
// ============================================================================

class GraphQLMutationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create a simple schema for testing
    SchemaType personType("Person", "http://example.org/Person");
    personType.addField(SchemaField("name", "http://example.org/name",
                                    ScalarType::STRING, false, true));
    personType.addField(SchemaField("email", "http://example.org/email",
                                    ScalarType::STRING, false, false));
    personType.addField(SchemaField("age", "http://example.org/age",
                                    ScalarType::INT, false, false));
    schema_.addType(std::move(personType));

    SchemaType organizationType("Organization", "http://example.org/Organization");
    organizationType.addField(SchemaField("name", "http://example.org/name",
                                          ScalarType::STRING, false, true));
    schema_.addType(std::move(organizationType));

    // Build mutation schema
    MutationSchemaConfig mutConfig;
    mutConfig.enableCreate = true;
    mutConfig.enableUpdate = true;
    mutConfig.enableDelete = true;
    mutConfig.enableUpsert = true;
    mutConfig.enableBatchCreate = true;
    mutConfig.enableBatchDelete = true;
    mutConfig.enableVersioning = true;
    mutConfig.enableTimestamps = true;
    MutationSchemaBuilder builder(mutConfig);
    builder.buildMutationSchema(schema_);
  }

  GraphQLSchema schema_;
};

// ============================================================================
// MutationSchemaBuilder Tests
// ============================================================================

TEST_F(GraphQLMutationTest, BuildMutationSchema_GeneratesMutations) {
  // Check that mutations were generated for Person type
  EXPECT_NE(schema_.findMutation("createPerson"), nullptr);
  EXPECT_NE(schema_.findMutation("updatePerson"), nullptr);
  EXPECT_NE(schema_.findMutation("deletePerson"), nullptr);
  EXPECT_NE(schema_.findMutation("upsertPerson"), nullptr);
  EXPECT_NE(schema_.findMutation("createPersons"), nullptr);
  EXPECT_NE(schema_.findMutation("deletePersons"), nullptr);
}

TEST_F(GraphQLMutationTest, BuildMutationSchema_GeneratesInputTypes) {
  // Check that input types were generated
  EXPECT_NE(schema_.findInputType("CreatePersonInput"), nullptr);
  EXPECT_NE(schema_.findInputType("UpdatePersonInput"), nullptr);
}

TEST_F(GraphQLMutationTest, BuildMutationSchema_CreateInputHasRequiredFields) {
  const auto* input = schema_.findInputType("CreatePersonInput");
  ASSERT_NE(input, nullptr);

  // Name is required (marked as required in schema)
  const auto* nameField = input->findField("name");
  ASSERT_NE(nameField, nullptr);
  EXPECT_TRUE(nameField->isRequired);

  // Email is not required
  const auto* emailField = input->findField("email");
  ASSERT_NE(emailField, nullptr);
  EXPECT_FALSE(emailField->isRequired);
}

TEST_F(GraphQLMutationTest, BuildMutationSchema_UpdateInputHasOptionalFields) {
  const auto* input = schema_.findInputType("UpdatePersonInput");
  ASSERT_NE(input, nullptr);

  // All fields should be optional for updates
  const auto* nameField = input->findField("name");
  ASSERT_NE(nameField, nullptr);
  EXPECT_FALSE(nameField->isRequired);
}

// ============================================================================
// GraphQLMutationTranslator Tests
// ============================================================================

TEST_F(GraphQLMutationTest, TranslateCreateMutation_GeneratesInsertData) {
  const char* mutation = R"(
    mutation {
      createPerson(input: { name: "Alice", email: "alice@example.com" }) {
        id
        name
      }
    }
  )";

  auto parseResult = GraphQLParser::parse(mutation);
  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLMutationTranslator translator(schema_);
  auto result = translator.translate(doc);

  ASSERT_TRUE(std::holds_alternative<MutationTranslationResult>(result));
  auto& mutResult = std::get<MutationTranslationResult>(result);

  EXPECT_EQ(mutResult.mutationType, MutationType::Create);
  EXPECT_EQ(mutResult.entityTypeName, "Person");
  EXPECT_FALSE(mutResult.entityIri.empty());
  EXPECT_FALSE(mutResult.statements.empty());
}

TEST_F(GraphQLMutationTest, TranslateUpdateMutation_GeneratesDeleteInsert) {
  const char* mutation = R"(
    mutation {
      updatePerson(id: "http://example.org/person/1", input: { name: "Bob" }) {
        id
        name
      }
    }
  )";

  auto parseResult = GraphQLParser::parse(mutation);
  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLMutationTranslator translator(schema_);
  auto result = translator.translate(doc);

  ASSERT_TRUE(std::holds_alternative<MutationTranslationResult>(result));
  auto& mutResult = std::get<MutationTranslationResult>(result);

  EXPECT_EQ(mutResult.mutationType, MutationType::Update);
  EXPECT_EQ(mutResult.entityIri, "http://example.org/person/1");
}

TEST_F(GraphQLMutationTest, TranslateDeleteMutation_GeneratesDeleteWhere) {
  const char* mutation = R"(
    mutation {
      deletePerson(id: "http://example.org/person/1") {
        success
      }
    }
  )";

  auto parseResult = GraphQLParser::parse(mutation);
  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLMutationTranslator translator(schema_);
  auto result = translator.translate(doc);

  ASSERT_TRUE(std::holds_alternative<MutationTranslationResult>(result));
  auto& mutResult = std::get<MutationTranslationResult>(result);

  EXPECT_EQ(mutResult.mutationType, MutationType::Delete);
  EXPECT_EQ(mutResult.entityIri, "http://example.org/person/1");
}

TEST_F(GraphQLMutationTest, TranslateBatchCreate_GeneratesMultipleTriples) {
  const char* mutation = R"(
    mutation {
      createPersons(input: [
        { name: "Alice" },
        { name: "Bob" },
        { name: "Charlie" }
      ]) {
        id
        name
      }
    }
  )";

  auto parseResult = GraphQLParser::parse(mutation);
  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLMutationTranslator translator(schema_);
  auto result = translator.translate(doc);

  ASSERT_TRUE(std::holds_alternative<MutationTranslationResult>(result));
  auto& mutResult = std::get<MutationTranslationResult>(result);

  EXPECT_EQ(mutResult.mutationType, MutationType::BatchCreate);
  EXPECT_EQ(mutResult.batchEntityIris.size(), 3u);
}

// ============================================================================
// IRI Generation Tests
// ============================================================================

TEST_F(GraphQLMutationTest, GenerateIri_UUID_GeneratesValidIri) {
  GraphQLMutationTranslator translator(schema_);

  std::string iri1 = translator.generateIri("Person", {});
  std::string iri2 = translator.generateIri("Person", {});

  // IRIs should be different (UUID)
  EXPECT_NE(iri1, iri2);

  // IRIs should contain the type name
  EXPECT_NE(iri1.find("Person"), std::string::npos);
}

TEST_F(GraphQLMutationTest, GetCurrentTimestamp_ReturnsValidFormat) {
  std::string timestamp = GraphQLMutationTranslator::getCurrentTimestamp();

  // Should be in ISO 8601 format: YYYY-MM-DDTHH:MM:SSZ
  EXPECT_EQ(timestamp.size(), 20u);
  EXPECT_EQ(timestamp[4], '-');
  EXPECT_EQ(timestamp[7], '-');
  EXPECT_EQ(timestamp[10], 'T');
  EXPECT_EQ(timestamp[13], ':');
  EXPECT_EQ(timestamp[16], ':');
  EXPECT_EQ(timestamp[19], 'Z');
}

// ============================================================================
// SPARQL Statement Serialization Tests
// ============================================================================

TEST_F(GraphQLMutationTest, InsertDataStatement_ToSparql) {
  InsertDataStatement stmt;
  stmt.triples.push_back(MutationTriple{
      "http://example.org/person/1",
      "http://www.w3.org/1999/02/22-rdf-syntax-ns#type",
      "http://example.org/Person"});
  stmt.triples.push_back(MutationTriple{
      "http://example.org/person/1",
      "http://example.org/name",
      "Alice",
      true});

  std::string sparql = stmt.toSparql();

  EXPECT_NE(sparql.find("INSERT DATA"), std::string::npos);
  EXPECT_NE(sparql.find("<http://example.org/person/1>"), std::string::npos);
  EXPECT_NE(sparql.find("\"Alice\""), std::string::npos);
}

TEST_F(GraphQLMutationTest, DeleteWhereStatement_ToSparql) {
  DeleteWhereStatement stmt;
  stmt.patterns.push_back(MutationTriple{
      "http://example.org/person/1",
      "http://example.org/name",
      "?old_name"});

  std::string sparql = stmt.toSparql();

  EXPECT_NE(sparql.find("DELETE WHERE"), std::string::npos);
  EXPECT_NE(sparql.find("<http://example.org/person/1>"), std::string::npos);
}

TEST_F(GraphQLMutationTest, DeleteInsertStatement_ToSparql) {
  DeleteInsertStatement stmt;
  stmt.deletePatterns.push_back(MutationTriple{
      "http://example.org/person/1",
      "http://example.org/name",
      "?old_name"});
  stmt.insertPatterns.push_back(MutationTriple{
      "http://example.org/person/1",
      "http://example.org/name",
      "Alice",
      true});
  stmt.wherePatterns.push_back(MutationTriple{
      "http://example.org/person/1",
      "http://www.w3.org/1999/02/22-rdf-syntax-ns#type",
      "http://example.org/Person"});

  std::string sparql = stmt.toSparql();

  EXPECT_NE(sparql.find("DELETE"), std::string::npos);
  EXPECT_NE(sparql.find("INSERT"), std::string::npos);
  EXPECT_NE(sparql.find("WHERE"), std::string::npos);
}

// ============================================================================
// MutationValidator Tests
// ============================================================================

TEST_F(GraphQLMutationTest, ValidateCreateInput_RequiredFieldMissing) {
  MutationValidator validator(schema_);

  // Missing required 'name' field
  nlohmann::json input = {{"email", "alice@example.com"}};

  auto result = validator.validateCreateInput("Person", input);

  EXPECT_FALSE(result.valid);
  EXPECT_FALSE(result.errors.empty());
}

TEST_F(GraphQLMutationTest, ValidateCreateInput_ValidInput) {
  MutationValidator validator(schema_);

  nlohmann::json input = {
      {"name", "Alice"},
      {"email", "alice@example.com"}};

  auto result = validator.validateCreateInput("Person", input);

  EXPECT_TRUE(result.valid);
}

TEST_F(GraphQLMutationTest, ValidateUpdateInput_AllOptional) {
  MutationValidator validator(schema_);

  // Even required fields are optional for updates
  nlohmann::json input = {{"email", "newemail@example.com"}};

  auto result = validator.validateUpdateInput("Person", input);

  EXPECT_TRUE(result.valid);
}

TEST_F(GraphQLMutationTest, ValidateCreateInput_InvalidScalarType) {
  MutationValidator validator(schema_);

  // Age should be an integer
  nlohmann::json input = {
      {"name", "Alice"},
      {"age", "not a number"}};

  auto result = validator.validateCreateInput("Person", input);

  EXPECT_FALSE(result.valid);
}

TEST_F(GraphQLMutationTest, ValidateDeleteInput_EmptyId) {
  MutationValidator validator(schema_);

  auto result = validator.validateDeleteInput("Person", "");

  EXPECT_FALSE(result.valid);
}

TEST_F(GraphQLMutationTest, ValidateDeleteInput_ValidId) {
  MutationValidator validator(schema_);

  auto result = validator.validateDeleteInput("Person", "http://example.org/person/1");

  EXPECT_TRUE(result.valid);
}

TEST_F(GraphQLMutationTest, ValidateBatchCreateInput_ValidArray) {
  MutationValidator validator(schema_);

  nlohmann::json inputs = nlohmann::json::array({
      {{"name", "Alice"}},
      {{"name", "Bob"}}});

  auto result = validator.validateBatchCreateInput("Person", inputs);

  EXPECT_TRUE(result.valid);
}

TEST_F(GraphQLMutationTest, ValidateBatchCreateInput_MixedValidity) {
  MutationValidator validator(schema_);

  // First is valid, second is missing required field
  nlohmann::json inputs = nlohmann::json::array({
      {{"name", "Alice"}},
      {{"email", "bob@example.com"}}});  // Missing 'name'

  auto result = validator.validateBatchCreateInput("Person", inputs);

  EXPECT_FALSE(result.valid);
  // Should have error with array index in path
  bool hasIndexedError = false;
  for (const auto& error : result.errors) {
    if (error.field.find("[1]") != std::string::npos) {
      hasIndexedError = true;
      break;
    }
  }
  EXPECT_TRUE(hasIndexedError);
}

// ============================================================================
// MutationTriple Tests
// ============================================================================

TEST_F(GraphQLMutationTest, MutationTriple_ToNTriples_IRI) {
  MutationTriple triple(
      "http://example.org/person/1",
      "http://example.org/knows",
      "http://example.org/person/2",
      false);

  std::string nt = triple.toNTriples();

  EXPECT_NE(nt.find("<http://example.org/person/1>"), std::string::npos);
  EXPECT_NE(nt.find("<http://example.org/knows>"), std::string::npos);
  EXPECT_NE(nt.find("<http://example.org/person/2>"), std::string::npos);
  EXPECT_NE(nt.find(" ."), std::string::npos);
}

TEST_F(GraphQLMutationTest, MutationTriple_ToNTriples_Literal) {
  MutationTriple triple(
      "http://example.org/person/1",
      "http://example.org/name",
      "Alice",
      true,
      "http://www.w3.org/2001/XMLSchema#string");

  std::string nt = triple.toNTriples();

  EXPECT_NE(nt.find("\"Alice\""), std::string::npos);
  EXPECT_NE(nt.find("^^<http://www.w3.org/2001/XMLSchema#string>"), std::string::npos);
}

TEST_F(GraphQLMutationTest, MutationTriple_ToNTriples_LangTag) {
  MutationTriple triple(
      "http://example.org/person/1",
      "http://example.org/name",
      "Alice",
      true,
      "",
      "en");

  std::string nt = triple.toNTriples();

  EXPECT_NE(nt.find("\"Alice\"@en"), std::string::npos);
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(GraphQLMutationTest, TranslateUnknownMutation_ReturnsError) {
  const char* mutation = R"(
    mutation {
      unknownMutation(input: {}) {
        id
      }
    }
  )";

  auto parseResult = GraphQLParser::parse(mutation);
  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLMutationTranslator translator(schema_);
  auto result = translator.translate(doc);

  EXPECT_TRUE(std::holds_alternative<std::vector<GraphQLError>>(result));
}

TEST_F(GraphQLMutationTest, TranslateQuery_NotMutation_ReturnsError) {
  const char* query = R"(
    query {
      Person {
        id
        name
      }
    }
  )";

  auto parseResult = GraphQLParser::parse(query);
  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  GraphQLMutationTranslator translator(schema_);
  auto result = translator.translate(doc);

  // Should return error because no mutation operation found
  EXPECT_TRUE(std::holds_alternative<std::vector<GraphQLError>>(result));
}
