// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifndef QLEVER_SRC_ENGINE_GRAPHQL_GRAPHQLRESULTFORMATTER_H
#define QLEVER_SRC_ENGINE_GRAPHQL_GRAPHQLRESULTFORMATTER_H

#ifdef QLEVER_GRAPHQL_SUPPORT

#include <string>
#include <unordered_map>
#include <vector>

#include "engine/Result.h"
#include "index/Index.h"
#include "parser/graphql/GraphQLParser.h"
#include "parser/graphql/GraphQLToSparql.h"
#include "util/json.h"

namespace graphql {

/// Metadata about a field in the GraphQL query, used for result formatting
struct FieldMapping {
  /// The SPARQL variable name (without ?)
  std::string sparqlVar;

  /// The GraphQL field name (response key)
  std::string graphqlKey;

  /// Whether this is a nested object field
  bool isNested = false;

  /// Parent field's SPARQL variable (for grouping nested results)
  std::string parentVar;

  /// Child field mappings (for nested objects)
  std::vector<FieldMapping> children;

  /// Whether this field represents a list (multiple values)
  bool isList = true;

  /// Type name for __typename introspection
  std::string typeName;
};

/// Transforms flat SPARQL query results into nested GraphQL JSON responses.
///
/// The GraphQL result format requires hierarchical nesting based on the
/// query structure, while SPARQL returns flat tabular results. This class
/// handles the transformation by:
/// 1. Grouping rows by parent entity IDs
/// 2. Collecting nested values into arrays
/// 3. Building the hierarchical JSON structure
///
/// Example:
/// SPARQL flat result:
///   ?person  | ?name      | ?knows   | ?knowsName
///   wd:Q937  | "Einstein" | wd:Q123  | "Bohr"
///   wd:Q937  | "Einstein" | wd:Q456  | "Planck"
///
/// GraphQL nested result:
///   {"Person": [{"id": "wd:Q937", "name": "Einstein",
///     "knows": [{"id": "wd:Q123", "name": "Bohr"},
///               {"id": "wd:Q456", "name": "Planck"}]}]}
class GraphQLResultFormatter {
 public:
  /// Format SPARQL results as a GraphQL response
  /// @param result The SPARQL query result
  /// @param translationResult The translation metadata from GraphQL->SPARQL
  /// @param operation The original GraphQL operation
  /// @param index The QLever index (for ID to string conversion)
  /// @return JSON object conforming to GraphQL response spec
  static nlohmann::json format(
      const Result& result,
      const TranslationResult& translationResult,
      const Operation& operation,
      const Index& index);

  /// Format errors as a GraphQL error response
  /// @param errors List of GraphQL errors
  /// @return JSON object with "errors" array
  static nlohmann::json formatErrors(
      const std::vector<GraphQLError>& errors);

  /// Create a GraphQL response with both data and errors
  /// @param data The data object (can be null)
  /// @param errors Optional list of errors
  /// @return Complete GraphQL response JSON
  static nlohmann::json createResponse(
      const nlohmann::json& data,
      const std::vector<GraphQLError>& errors = {});

 private:
  /// Build field mappings from the GraphQL operation and translation result
  static std::vector<FieldMapping> buildFieldMappings(
      const Operation& operation,
      const TranslationResult& translationResult);

  /// Build field mapping for a single field
  static FieldMapping buildFieldMapping(
      const Field& field,
      const TranslationResult& translationResult,
      const std::string& parentVar,
      const std::string& parentTypeName = "");

  /// Convert a single result row to JSON values
  static void processRow(
      const std::vector<std::string>& row,
      const std::vector<std::string>& columnNames,
      const std::vector<FieldMapping>& mappings,
      std::unordered_map<std::string, nlohmann::json>& entityMap);

  /// Build nested JSON structure from entity map
  static nlohmann::json buildNestedResult(
      const std::unordered_map<std::string, nlohmann::json>& entityMap,
      const std::vector<FieldMapping>& mappings);

  /// Convert a SPARQL value to appropriate JSON type
  static nlohmann::json sparqlValueToJson(const std::string& value);

  /// Extract the local name from an IRI
  static std::string extractLocalName(const std::string& iri);

  /// Check if a string looks like an IRI
  static bool isIRI(const std::string& value);

  /// Check if a string is a literal with datatype
  static bool isTypedLiteral(const std::string& value);

  /// Parse a typed literal and return appropriate JSON value
  static nlohmann::json parseTypedLiteral(const std::string& value);
};

}  // namespace graphql

#endif  // QLEVER_GRAPHQL_SUPPORT
#endif  // QLEVER_SRC_ENGINE_GRAPHQL_GRAPHQLRESULTFORMATTER_H
