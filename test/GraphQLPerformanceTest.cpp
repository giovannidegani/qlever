// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <numeric>
#include <vector>

#include "engine/graphql/MutationSchemaBuilder.h"
#include "parser/graphql/GraphQLMutationTranslator.h"
#include "parser/graphql/GraphQLParser.h"
#include "parser/graphql/GraphQLToSparql.h"
#include "parser/graphql/MutationValidator.h"

using namespace graphql;
using namespace std::chrono;

// ============================================================================
// Performance Test Utilities
// ============================================================================

struct BenchmarkResult {
  std::string name;
  size_t iterations;
  double totalMs;
  double avgMs;
  double minMs;
  double maxMs;
  double stdDevMs;
  size_t opsPerSecond;
};

template <typename Func>
BenchmarkResult runBenchmark(const std::string& name, size_t iterations,
                             Func&& func) {
  std::vector<double> times;
  times.reserve(iterations);

  // Warmup
  for (size_t i = 0; i < std::min(iterations / 10, size_t(10)); ++i) {
    func();
  }

  // Actual benchmark
  for (size_t i = 0; i < iterations; ++i) {
    auto start = high_resolution_clock::now();
    func();
    auto end = high_resolution_clock::now();
    times.push_back(duration<double, std::milli>(end - start).count());
  }

  BenchmarkResult result;
  result.name = name;
  result.iterations = iterations;
  result.totalMs = std::accumulate(times.begin(), times.end(), 0.0);
  result.avgMs = result.totalMs / iterations;
  result.minMs = *std::min_element(times.begin(), times.end());
  result.maxMs = *std::max_element(times.begin(), times.end());

  // Standard deviation
  double sumSquaredDiff = 0.0;
  for (double t : times) {
    sumSquaredDiff += (t - result.avgMs) * (t - result.avgMs);
  }
  result.stdDevMs = std::sqrt(sumSquaredDiff / iterations);

  result.opsPerSecond = static_cast<size_t>(1000.0 / result.avgMs);

  return result;
}

void printBenchmarkResult(const BenchmarkResult& result) {
  std::cout << "\n=== " << result.name << " ===" << std::endl;
  std::cout << "  Iterations: " << result.iterations << std::endl;
  std::cout << "  Total time: " << result.totalMs << " ms" << std::endl;
  std::cout << "  Avg time:   " << result.avgMs << " ms" << std::endl;
  std::cout << "  Min time:   " << result.minMs << " ms" << std::endl;
  std::cout << "  Max time:   " << result.maxMs << " ms" << std::endl;
  std::cout << "  Std dev:    " << result.stdDevMs << " ms" << std::endl;
  std::cout << "  Throughput: " << result.opsPerSecond << " ops/sec" << std::endl;
}

// ============================================================================
// Test Fixture
// ============================================================================

class GraphQLPerformanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Build a realistic schema with multiple types
    for (int i = 0; i < 20; ++i) {
      SchemaType type("Type" + std::to_string(i),
                      "http://example.org/Type" + std::to_string(i));

      // Add scalar fields
      for (int j = 0; j < 10; ++j) {
        type.addField(SchemaField(
            "field" + std::to_string(j),
            "http://example.org/field" + std::to_string(j),
            ScalarType::STRING, false, j == 0));  // First field required
      }

      // Add some numeric fields
      type.addField(SchemaField("count", "http://example.org/count",
                                ScalarType::INT, false, false));
      type.addField(SchemaField("score", "http://example.org/score",
                                ScalarType::FLOAT, false, false));

      // Add relation fields (to other types)
      if (i > 0) {
        type.addField(SchemaField(
            "relatedTo", "http://example.org/relatedTo",
            "Type" + std::to_string(i - 1), true, false));
      }

      schema_.addType(std::move(type));
    }

    // Build mutation schema
    MutationSchemaConfig mutConfig;
    mutConfig.enableCreate = true;
    mutConfig.enableUpdate = true;
    mutConfig.enableDelete = true;
    mutConfig.enableBatchCreate = true;
    MutationSchemaBuilder builder(mutConfig);
    builder.buildMutationSchema(schema_);
  }

  GraphQLSchema schema_;

  // Test queries of various complexity
  const std::string simpleQuery_ = R"(
    query {
      Type0 {
        id
        field0
        field1
      }
    }
  )";

  const std::string mediumQuery_ = R"(
    query {
      Type5(filter: { field0: { contains: "test" } }, first: 100) {
        id
        field0
        field1
        field2
        field3
        count
        score
        relatedTo {
          id
          field0
        }
      }
    }
  )";

  const std::string complexQuery_ = R"(
    query GetData {
      Type10(filter: {
        field0: { contains: "test" }
        count: { gt: 10 }
      }, first: 50, orderBy: "field0") {
        id
        field0
        field1
        field2
        field3
        field4
        count
        score
        relatedTo {
          id
          field0
          field1
          relatedTo {
            id
            field0
          }
        }
      }
    }
  )";

  const std::string queryWithFragments_ = R"(
    fragment TypeFields on Type5 {
      id
      field0
      field1
      field2
      count
    }

    query {
      Type5 {
        ...TypeFields
        relatedTo {
          ...TypeFields
        }
      }
    }
  )";

  const std::string simpleMutation_ = R"(
    mutation {
      createType0(input: { field0: "test", field1: "value" }) {
        id
        field0
      }
    }
  )";

  const std::string batchMutation_ = R"(
    mutation {
      createType0s(input: [
        { field0: "test1" },
        { field0: "test2" },
        { field0: "test3" },
        { field0: "test4" },
        { field0: "test5" }
      ]) {
        id
        field0
      }
    }
  )";
};

// ============================================================================
// Parser Performance Tests
// ============================================================================

TEST_F(GraphQLPerformanceTest, ParseSimpleQuery) {
  auto result = runBenchmark("Parse Simple Query", 10000, [this]() {
    auto parseResult = GraphQLParser::parse(simpleQuery_);
    EXPECT_TRUE(std::holds_alternative<Document>(parseResult));
  });
  printBenchmarkResult(result);

  // Assert reasonable performance (should parse > 10000 queries/sec)
  EXPECT_GT(result.opsPerSecond, 10000u);
}

TEST_F(GraphQLPerformanceTest, ParseMediumQuery) {
  auto result = runBenchmark("Parse Medium Query", 10000, [this]() {
    auto parseResult = GraphQLParser::parse(mediumQuery_);
    EXPECT_TRUE(std::holds_alternative<Document>(parseResult));
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 5000u);
}

TEST_F(GraphQLPerformanceTest, ParseComplexQuery) {
  auto result = runBenchmark("Parse Complex Query", 5000, [this]() {
    auto parseResult = GraphQLParser::parse(complexQuery_);
    EXPECT_TRUE(std::holds_alternative<Document>(parseResult));
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 2000u);
}

TEST_F(GraphQLPerformanceTest, ParseQueryWithFragments) {
  auto result = runBenchmark("Parse Query with Fragments", 5000, [this]() {
    auto parseResult = GraphQLParser::parse(queryWithFragments_);
    EXPECT_TRUE(std::holds_alternative<Document>(parseResult));
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 3000u);
}

TEST_F(GraphQLPerformanceTest, ParseMutation) {
  auto result = runBenchmark("Parse Simple Mutation", 10000, [this]() {
    auto parseResult = GraphQLParser::parse(simpleMutation_);
    EXPECT_TRUE(std::holds_alternative<Document>(parseResult));
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 10000u);
}

// ============================================================================
// Translation Performance Tests
// ============================================================================

TEST_F(GraphQLPerformanceTest, TranslateSimpleQuery) {
  auto parseResult = GraphQLParser::parse(simpleQuery_);
  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  auto result = runBenchmark("Translate Simple Query", 10000, [&]() {
    GraphQLToSparql translator(schema_);
    auto translateResult = translator.translate(doc);
    EXPECT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 5000u);
}

TEST_F(GraphQLPerformanceTest, TranslateMediumQuery) {
  auto parseResult = GraphQLParser::parse(mediumQuery_);
  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  auto result = runBenchmark("Translate Medium Query", 5000, [&]() {
    GraphQLToSparql translator(schema_);
    auto translateResult = translator.translate(doc);
    EXPECT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 1000u);  // Relaxed for complex queries
}

TEST_F(GraphQLPerformanceTest, TranslateComplexQuery) {
  auto parseResult = GraphQLParser::parse(complexQuery_);
  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  auto result = runBenchmark("Translate Complex Query", 2000, [&]() {
    GraphQLToSparql translator(schema_);
    auto translateResult = translator.translate(doc);
    EXPECT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 500u);
}

// ============================================================================
// Mutation Translation Performance Tests
// ============================================================================

TEST_F(GraphQLPerformanceTest, TranslateSimpleMutation) {
  auto parseResult = GraphQLParser::parse(simpleMutation_);
  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  auto result = runBenchmark("Translate Simple Mutation", 10000, [&]() {
    GraphQLMutationTranslator translator(schema_);
    auto translateResult = translator.translate(doc);
    EXPECT_TRUE(std::holds_alternative<MutationTranslationResult>(translateResult));
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 5000u);
}

TEST_F(GraphQLPerformanceTest, TranslateBatchMutation) {
  auto parseResult = GraphQLParser::parse(batchMutation_);
  ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
  const auto& doc = std::get<Document>(parseResult);

  auto result = runBenchmark("Translate Batch Mutation (5 items)", 5000, [&]() {
    GraphQLMutationTranslator translator(schema_);
    auto translateResult = translator.translate(doc);
    EXPECT_TRUE(std::holds_alternative<MutationTranslationResult>(translateResult));
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 2000u);
}

// ============================================================================
// Validation Performance Tests
// ============================================================================

TEST_F(GraphQLPerformanceTest, ValidateCreateInput) {
  nlohmann::json input = {
      {"field0", "test value"},
      {"field1", "another value"},
      {"field2", "third value"},
      {"count", 42},
      {"score", 3.14}};

  auto result = runBenchmark("Validate Create Input", 50000, [&]() {
    MutationValidator validator(schema_);
    auto validationResult = validator.validateCreateInput("Type0", input);
    EXPECT_TRUE(validationResult.valid);
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 20000u);
}

TEST_F(GraphQLPerformanceTest, ValidateBatchInput) {
  nlohmann::json inputs = nlohmann::json::array();
  for (int i = 0; i < 100; ++i) {
    inputs.push_back({
        {"field0", "test " + std::to_string(i)},
        {"field1", "value " + std::to_string(i)}});
  }

  auto result = runBenchmark("Validate Batch Input (100 items)", 1000, [&]() {
    MutationValidator validator(schema_);
    auto validationResult = validator.validateBatchCreateInput("Type0", inputs);
    EXPECT_TRUE(validationResult.valid);
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 100u);
}

// ============================================================================
// Schema Building Performance Tests
// ============================================================================

TEST_F(GraphQLPerformanceTest, BuildMutationSchema) {
  auto result = runBenchmark("Build Mutation Schema (20 types)", 1000, [&]() {
    GraphQLSchema freshSchema;

    // Create schema with 20 types
    for (int i = 0; i < 20; ++i) {
      SchemaType type("Type" + std::to_string(i),
                      "http://example.org/Type" + std::to_string(i));
      for (int j = 0; j < 10; ++j) {
        type.addField(SchemaField(
            "field" + std::to_string(j),
            "http://example.org/field" + std::to_string(j),
            ScalarType::STRING, false, j == 0));
      }
      freshSchema.addType(std::move(type));
    }

    MutationSchemaConfig config;
    config.enableCreate = true;
    config.enableUpdate = true;
    config.enableDelete = true;
    config.enableBatchCreate = true;
    config.enableBatchDelete = true;
    MutationSchemaBuilder builder(config);
    builder.buildMutationSchema(freshSchema);

    EXPECT_TRUE(freshSchema.hasMutations());
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 500u);
}

// ============================================================================
// IRI Generation Performance Tests
// ============================================================================

TEST_F(GraphQLPerformanceTest, GenerateUUIDs) {
  GraphQLMutationTranslator translator(schema_);

  auto result = runBenchmark("Generate UUID IRIs", 100000, [&]() {
    std::string iri = translator.generateIri("Type0", {});
    EXPECT_FALSE(iri.empty());
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 50000u);
}

// ============================================================================
// SPARQL Statement Serialization Performance
// ============================================================================

TEST_F(GraphQLPerformanceTest, SerializeInsertData) {
  InsertDataStatement stmt;
  for (int i = 0; i < 100; ++i) {
    stmt.triples.push_back(MutationTriple{
        "http://example.org/entity/" + std::to_string(i),
        "http://example.org/property",
        "value " + std::to_string(i),
        true,
        "http://www.w3.org/2001/XMLSchema#string"});
  }

  auto result = runBenchmark("Serialize INSERT DATA (100 triples)", 10000, [&]() {
    std::string sparql = stmt.toSparql();
    EXPECT_FALSE(sparql.empty());
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 5000u);
}

// ============================================================================
// End-to-End Performance Tests
// ============================================================================

TEST_F(GraphQLPerformanceTest, EndToEndQueryProcessing) {
  // Measure full pipeline: parse + translate
  auto result = runBenchmark("E2E Query Processing", 5000, [this]() {
    // Parse
    auto parseResult = GraphQLParser::parse(mediumQuery_);
    ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
    const auto& doc = std::get<Document>(parseResult);

    // Translate
    GraphQLToSparql translator(schema_);
    auto translateResult = translator.translate(doc);
    EXPECT_TRUE(std::holds_alternative<TranslationResult>(translateResult));
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 1000u);
}

TEST_F(GraphQLPerformanceTest, EndToEndMutationProcessing) {
  // Measure full mutation pipeline: parse + validate + translate
  nlohmann::json inputData = {
      {"field0", "test"},
      {"field1", "value"}};

  auto result = runBenchmark("E2E Mutation Processing", 5000, [&]() {
    // Parse
    auto parseResult = GraphQLParser::parse(simpleMutation_);
    ASSERT_TRUE(std::holds_alternative<Document>(parseResult));
    const auto& doc = std::get<Document>(parseResult);

    // Validate
    MutationValidator validator(schema_);
    auto validationResult = validator.validateCreateInput("Type0", inputData);
    EXPECT_TRUE(validationResult.valid);

    // Translate
    GraphQLMutationTranslator translator(schema_);
    auto translateResult = translator.translate(doc);
    EXPECT_TRUE(std::holds_alternative<MutationTranslationResult>(translateResult));
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 1000u);
}

// ============================================================================
// Memory Stress Tests
// ============================================================================

TEST_F(GraphQLPerformanceTest, ParseLargeQuery) {
  // Generate a query with many fields
  std::string largeQuery = "query {\n  Type0 {\n    id\n";
  for (int i = 0; i < 100; ++i) {
    largeQuery += "    alias" + std::to_string(i) + ": field" +
                  std::to_string(i % 10) + "\n";
  }
  largeQuery += "  }\n}";

  auto result = runBenchmark("Parse Large Query (100 fields)", 1000, [&]() {
    auto parseResult = GraphQLParser::parse(largeQuery);
    EXPECT_TRUE(std::holds_alternative<Document>(parseResult));
  });
  printBenchmarkResult(result);

  EXPECT_GT(result.opsPerSecond, 500u);
}

TEST_F(GraphQLPerformanceTest, ValidateLargeBatch) {
  // Generate large batch input
  nlohmann::json largeBatch = nlohmann::json::array();
  for (int i = 0; i < 1000; ++i) {
    largeBatch.push_back({
        {"field0", "test " + std::to_string(i)},
        {"field1", "value " + std::to_string(i)},
        {"field2", "data " + std::to_string(i)},
        {"count", i},
        {"score", static_cast<double>(i) / 100.0}});
  }

  auto result = runBenchmark("Validate Large Batch (1000 items)", 100, [&]() {
    MutationValidator validator(schema_);
    auto validationResult = validator.validateBatchCreateInput("Type0", largeBatch);
    EXPECT_TRUE(validationResult.valid);
  });
  printBenchmarkResult(result);

  // Should handle 1000-item batches at > 10/sec
  EXPECT_GT(result.opsPerSecond, 10u);
}

// ============================================================================
// Comparison Summary Test
// ============================================================================

TEST_F(GraphQLPerformanceTest, PerformanceSummary) {
  std::cout << "\n" << std::string(60, '=') << std::endl;
  std::cout << "GraphQL Performance Summary" << std::endl;
  std::cout << std::string(60, '=') << std::endl;

  // Quick benchmarks for summary
  auto parseSimple = runBenchmark("Parse (simple)", 1000, [this]() {
    GraphQLParser::parse(simpleQuery_);
  });

  auto parseComplex = runBenchmark("Parse (complex)", 1000, [this]() {
    GraphQLParser::parse(complexQuery_);
  });

  auto parseResult = GraphQLParser::parse(mediumQuery_);
  const auto& doc = std::get<Document>(parseResult);

  auto translateQuery = runBenchmark("Translate query", 1000, [&]() {
    GraphQLToSparql translator(schema_);
    translator.translate(doc);
  });

  auto parseMut = GraphQLParser::parse(simpleMutation_);
  const auto& mutDoc = std::get<Document>(parseMut);

  auto translateMutation = runBenchmark("Translate mutation", 1000, [&]() {
    GraphQLMutationTranslator translator(schema_);
    translator.translate(mutDoc);
  });

  std::cout << "\nOperation                  | Avg (ms) | Ops/sec" << std::endl;
  std::cout << std::string(60, '-') << std::endl;
  std::cout << "Parse simple query         | " << std::fixed << std::setprecision(3)
            << parseSimple.avgMs << "    | " << parseSimple.opsPerSecond << std::endl;
  std::cout << "Parse complex query        | " << parseComplex.avgMs << "    | "
            << parseComplex.opsPerSecond << std::endl;
  std::cout << "Translate query            | " << translateQuery.avgMs << "    | "
            << translateQuery.opsPerSecond << std::endl;
  std::cout << "Translate mutation         | " << translateMutation.avgMs << "    | "
            << translateMutation.opsPerSecond << std::endl;
  std::cout << std::string(60, '=') << std::endl;
}
