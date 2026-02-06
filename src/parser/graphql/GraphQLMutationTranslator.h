// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifndef QLEVER_SRC_PARSER_GRAPHQL_GRAPHQLMUTATIONTRANSLATOR_H
#define QLEVER_SRC_PARSER_GRAPHQL_GRAPHQLMUTATIONTRANSLATOR_H

#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "parser/graphql/GraphQLParser.h"
#include "parser/graphql/GraphQLSchema.h"
#include "util/json.h"

namespace graphql {

// ============================================================================
// SPARQL Update Statement Types
// ============================================================================

/// A single RDF triple for mutations
struct MutationTriple {
  std::string subject;
  std::string predicate;
  std::string object;
  bool objectIsLiteral = false;
  std::string datatypeIri;  // For typed literals
  std::string langTag;      // For language-tagged strings

  MutationTriple() = default;

  MutationTriple(std::string s, std::string p, std::string o,
                 bool isLiteral = false)
      : subject(std::move(s)),
        predicate(std::move(p)),
        object(std::move(o)),
        objectIsLiteral(isLiteral) {}

  // Full constructor with datatype
  MutationTriple(std::string s, std::string p, std::string o,
                 bool isLiteral, std::string datatype)
      : subject(std::move(s)),
        predicate(std::move(p)),
        object(std::move(o)),
        objectIsLiteral(isLiteral),
        datatypeIri(std::move(datatype)) {}

  // Full constructor with datatype and language tag
  MutationTriple(std::string s, std::string p, std::string o,
                 bool isLiteral, std::string datatype, std::string lang)
      : subject(std::move(s)),
        predicate(std::move(p)),
        object(std::move(o)),
        objectIsLiteral(isLiteral),
        datatypeIri(std::move(datatype)),
        langTag(std::move(lang)) {}

  // Serialize to N-Triples format
  std::string toNTriples() const;
};

/// Represents a SPARQL INSERT DATA statement
struct InsertDataStatement {
  std::vector<MutationTriple> triples;
  std::optional<std::string> graph;  // Named graph (optional)

  std::string toSparql() const;
};

/// Represents a SPARQL DELETE WHERE statement
struct DeleteWhereStatement {
  std::vector<MutationTriple> patterns;  // Can include variables
  std::optional<std::string> graph;

  std::string toSparql() const;
};

/// Represents a SPARQL DELETE/INSERT WHERE statement (for updates)
struct DeleteInsertStatement {
  std::vector<MutationTriple> deletePatterns;
  std::vector<MutationTriple> insertPatterns;
  std::vector<MutationTriple> wherePatterns;
  std::vector<std::string> filterExpressions;
  std::optional<std::string> graph;

  std::string toSparql() const;
};

/// Union type for all SPARQL Update statement types
using SparqlUpdateStatement =
    std::variant<InsertDataStatement, DeleteWhereStatement,
                 DeleteInsertStatement>;

// ============================================================================
// Mutation Translation Result
// ============================================================================

/// Result of translating a single mutation
struct MutationTranslationResult {
  /// The generated SPARQL Update statements (may be multiple for nested mutations)
  std::vector<SparqlUpdateStatement> statements;

  /// The IRI of the created/updated entity (for returning to client)
  std::string entityIri;

  /// Type of the entity being mutated
  std::string entityTypeName;

  /// Whether this was a create, update, delete, etc.
  MutationType mutationType;

  /// Fields that were selected for return (to query after mutation)
  std::vector<std::string> returnFields;

  /// Generated IRIs for batch creates (for BatchCreate)
  std::vector<std::string> batchEntityIris;
};

// ============================================================================
// GraphQL Mutation Translator
// ============================================================================

/// Translates GraphQL mutations to SPARQL Update statements
class GraphQLMutationTranslator {
 public:
  /// Constructor
  /// @param schema The GraphQL schema (with mutation definitions)
  explicit GraphQLMutationTranslator(const GraphQLSchema& schema);

  /// Translate a GraphQL mutation to SPARQL Update statements
  /// @param document The parsed GraphQL document
  /// @param operationName Optional operation name
  /// @param variables Variable values from the request
  /// @return Translation result or errors
  std::variant<MutationTranslationResult, std::vector<GraphQLError>> translate(
      const Document& document,
      std::optional<std::string_view> operationName = std::nullopt,
      const std::unordered_map<std::string, Value>& variables = {});

  /// Generate a new IRI for entity creation
  /// @param typeName The GraphQL type name
  /// @param inputData The input data (for template-based IRI generation)
  /// @return Generated IRI
  std::string generateIri(const std::string& typeName,
                          const nlohmann::json& inputData = {});

  /// Get current timestamp in XSD dateTime format
  static std::string getCurrentTimestamp();

 private:
  const GraphQLSchema& schema_;

  // IRI generation counter for uniqueness
  size_t iriCounter_ = 0;

  // ============================================================================
  // Core Translation Methods
  // ============================================================================

  /// Translate a single mutation field
  std::variant<MutationTranslationResult, std::vector<GraphQLError>>
  translateMutationField(const Field& field,
                         const std::unordered_map<std::string, Value>& variables);

  /// Translate a CREATE mutation
  MutationTranslationResult translateCreate(
      const MutationField& mutation, const Field& field,
      const std::unordered_map<std::string, Value>& variables);

  /// Translate an UPDATE mutation
  MutationTranslationResult translateUpdate(
      const MutationField& mutation, const Field& field,
      const std::unordered_map<std::string, Value>& variables);

  /// Translate a DELETE mutation
  MutationTranslationResult translateDelete(
      const MutationField& mutation, const Field& field,
      const std::unordered_map<std::string, Value>& variables);

  /// Translate an UPSERT mutation
  MutationTranslationResult translateUpsert(
      const MutationField& mutation, const Field& field,
      const std::unordered_map<std::string, Value>& variables);

  /// Translate a BATCH CREATE mutation
  MutationTranslationResult translateBatchCreate(
      const MutationField& mutation, const Field& field,
      const std::unordered_map<std::string, Value>& variables);

  /// Translate a BATCH DELETE mutation
  MutationTranslationResult translateBatchDelete(
      const MutationField& mutation, const Field& field,
      const std::unordered_map<std::string, Value>& variables);

  // ============================================================================
  // Helper Methods
  // ============================================================================

  /// Parse input data from GraphQL arguments
  nlohmann::json parseInputData(const Argument& arg,
                                const std::unordered_map<std::string, Value>& variables);

  /// Convert a GraphQL Value to nlohmann::json
  nlohmann::json valueToJson(const Value& value,
                             const std::unordered_map<std::string, Value>& variables);

  /// Build triples for creating an entity from input data
  std::vector<MutationTriple> buildCreateTriples(
      const std::string& subjectIri, const SchemaType& type,
      const nlohmann::json& inputData, const std::string& timestamp);

  /// Build triples for updating an entity from input data
  /// Returns pair of (delete patterns, insert patterns)
  std::pair<std::vector<MutationTriple>, std::vector<MutationTriple>>
  buildUpdateTriples(const std::string& subjectIri, const SchemaType& type,
                     const nlohmann::json& inputData,
                     const std::string& timestamp);

  /// Build delete patterns for an entity and all its properties
  std::vector<MutationTriple> buildDeleteAllTriples(
      const std::string& subjectIri, const SchemaType& type);

  /// Convert a JSON value to RDF literal format
  MutationTriple buildLiteralTriple(const std::string& subject,
                                    const std::string& predicate,
                                    const nlohmann::json& value,
                                    ScalarType scalarType);

  /// Get the XSD datatype IRI for a scalar type
  static std::string scalarTypeToXsdIri(ScalarType type);

  /// Handle relation input (connect, disconnect, create, etc.)
  std::vector<SparqlUpdateStatement> handleRelationInput(
      const std::string& subjectIri, const SchemaField& field,
      const nlohmann::json& relationInput, const std::string& timestamp);

  /// Extract ID from argument
  std::optional<std::string> extractId(
      const std::vector<Argument>& arguments,
      const std::unordered_map<std::string, Value>& variables);

  /// Extract input data argument
  std::optional<nlohmann::json> extractInputData(
      const std::vector<Argument>& arguments, const std::string& argName,
      const std::unordered_map<std::string, Value>& variables);

  /// Collect return fields from selection set
  std::vector<std::string> collectReturnFields(const Field& field);

  /// Build version check pattern (for optimistic locking)
  std::vector<MutationTriple> buildVersionCheckPattern(
      const std::string& subjectIri, int64_t expectedVersion);

  /// Build version increment triple
  MutationTriple buildVersionTriple(const std::string& subjectIri,
                                    int64_t newVersion);

  /// Escape IRI for SPARQL
  static std::string escapeIri(const std::string& iri);

 public:
  /// Escape literal string for SPARQL (public for use by MutationTriple)
  static std::string escapeLiteral(const std::string& str);
};

}  // namespace graphql

#endif  // QLEVER_SRC_PARSER_GRAPHQL_GRAPHQLMUTATIONTRANSLATOR_H
