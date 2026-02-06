// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifndef QLEVER_SRC_PARSER_GRAPHQL_GRAPHQLTOSPARQL_H
#define QLEVER_SRC_PARSER_GRAPHQL_GRAPHQLTOSPARQL_H

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "parser/ParsedQuery.h"
#include "parser/graphql/GraphQLParser.h"
#include "parser/graphql/GraphQLSchema.h"

namespace graphql {

/// Default pagination limits
constexpr uint64_t DEFAULT_MAX_RESULTS = 10000;
constexpr uint64_t DEFAULT_MAX_OFFSET = 1000000;

/// Configuration for GraphQL to SPARQL translation
struct TranslationConfig {
  /// Maximum number of results per query (first/limit argument)
  uint64_t maxResults = DEFAULT_MAX_RESULTS;

  /// Maximum offset value for pagination
  uint64_t maxOffset = DEFAULT_MAX_OFFSET;
};

/// Column info for variable-to-column mapping
struct ColumnInfo {
  size_t columnIndex_;
};

/// Result of translating a GraphQL query to SPARQL
struct TranslationResult {
  /// The translated ParsedQuery (for QLever's query planner)
  ParsedQuery parsedQuery;

  /// Mapping from SPARQL variables to GraphQL field paths
  /// e.g., "?person_name" -> ["persons", 0, "name"]
  std::unordered_map<std::string, std::vector<std::variant<std::string, size_t>>>
      variableToPath;

  /// Mapping from GraphQL field names to SPARQL variable names
  /// e.g., "name" -> "person_name"
  std::unordered_map<std::string, std::string> fieldToVariable;

  /// Mapping from SPARQL variable names to column indices
  /// (populated after query planning)
  std::unordered_map<std::string, ColumnInfo> variableToColumn;

  /// The structure needed to reconstruct nested JSON from flat results
  struct FieldMapping {
    std::string sparqlVar;      // The SPARQL variable name
    std::string graphqlField;   // The GraphQL field name
    std::string parentVar;      // Parent object's SPARQL variable
    bool isList;                // Is this field a list?
    ScalarType scalarType;      // Type if scalar
    std::string objectTypeName; // Type name if object
    std::vector<FieldMapping> children;  // Nested fields
  };
  FieldMapping rootMapping;
};

/// Translates GraphQL queries to SPARQL queries
class GraphQLToSparql {
 public:
  /// Constructor
  /// @param schema The GraphQL schema (derived from RDF)
  /// @param config Optional translation configuration
  explicit GraphQLToSparql(const GraphQLSchema& schema,
                           TranslationConfig config = {});

  /// Translate a GraphQL document to a ParsedQuery
  /// @param document The parsed GraphQL document
  /// @param operationName Optional operation name to translate
  /// @param variables Variable values from the request
  /// @return Translation result or errors
  std::variant<TranslationResult, std::vector<GraphQLError>> translate(
      const Document& document,
      std::optional<std::string_view> operationName = std::nullopt,
      const std::unordered_map<std::string, Value>& variables = {});

  /// Get the current configuration
  const TranslationConfig& config() const { return config_; }

  /// Update the configuration
  void setConfig(TranslationConfig config) { config_ = std::move(config); }

 private:
  const GraphQLSchema& schema_;
  TranslationConfig config_;

  // Variable name generation
  size_t varCounter_ = 0;
  std::string nextVar(const std::string& base = "v");
  void resetVarCounter() { varCounter_ = 0; }

  // Translation context
  struct TranslationContext {
    ParsedQuery& query;
    parsedQuery::GraphPattern& currentPattern;
    std::vector<Variable>& selectVariables;
    TranslationResult::FieldMapping& fieldMapping;
    std::string subjectVar;
    const SchemaType* parentType;
    std::vector<GraphQLError>& errors;
    const std::unordered_map<std::string, Value>& variables;
    const Document& document;
    // Security: Track expanded fragments to detect cycles
    std::unordered_set<std::string> expandedFragments;
  };

  // Core translation methods
  void translateOperation(const Operation& op,
                          const std::unordered_map<std::string, Value>& variables,
                          const Document& document,
                          TranslationResult& result,
                          std::vector<GraphQLError>& errors);

  void translateSelectionSet(const std::vector<Selection>& selections,
                             TranslationContext& ctx);

  void translateField(const Field& field, TranslationContext& ctx);

  void translateFragmentSpread(const FragmentSpread& spread,
                               TranslationContext& ctx);

  void translateInlineFragment(const InlineFragment& fragment,
                               TranslationContext& ctx);

  // Filter translation
  void translateFilter(const Argument& filterArg,
                       const std::string& subjectVar,
                       const SchemaType& type,
                       TranslationContext& ctx);

  void translateFilterField(const std::string& fieldName,
                            const Value& filterValue,
                            const std::string& subjectVar,
                            const SchemaType& type,
                            TranslationContext& ctx);

  void translateStringFilter(const std::string& varName,
                             const Value& filterValue,
                             TranslationContext& ctx);

  void translateNumericFilter(const std::string& varName,
                              const Value& filterValue,
                              bool isFloat,
                              TranslationContext& ctx);

  void translateIdFilter(const std::string& subjectVar,
                         const Value& filterValue,
                         TranslationContext& ctx);

  // Pagination and ordering
  void applyPagination(const std::vector<Argument>& arguments,
                       ParsedQuery& query);

  void applyOrdering(const std::vector<Argument>& arguments,
                     const std::string& subjectVar,
                     const SchemaType& type,
                     ParsedQuery& query);

  // Helper methods
  Value resolveValue(const Value& value,
                     const std::unordered_map<std::string, Value>& variables);

  std::string valueToSparqlLiteral(const Value& value);

  SparqlTriple makeTriple(const std::string& subject,
                          const std::string& predicate,
                          const std::string& object);

  void addBasicGraphPattern(parsedQuery::GraphPattern& pattern,
                            const SparqlTriple& triple);

  void addOptionalPattern(parsedQuery::GraphPattern& pattern,
                          const std::vector<SparqlTriple>& triples);

  void addFilter(parsedQuery::GraphPattern& pattern,
                 const std::string& expression);

  // Translate OR filter to SPARQL UNION
  void translateOrFilter(const Value& orFilters,
                         const std::string& subjectVar,
                         const SchemaType& type,
                         TranslationContext& ctx);

  // Translate NOT filter to SPARQL MINUS
  void translateNotFilter(const Value& notFilter,
                          const std::string& subjectVar,
                          const SchemaType& type,
                          TranslationContext& ctx);
};

}  // namespace graphql

#endif  // QLEVER_SRC_PARSER_GRAPHQL_GRAPHQLTOSPARQL_H
