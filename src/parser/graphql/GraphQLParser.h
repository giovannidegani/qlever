// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifndef QLEVER_SRC_PARSER_GRAPHQL_GRAPHQLPARSER_H
#define QLEVER_SRC_PARSER_GRAPHQL_GRAPHQLPARSER_H

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "util/json.h"

namespace graphql {

// ============================================================================
// GraphQL AST Types
// ============================================================================

/// Represents a location in the GraphQL source
struct SourceLocation {
  size_t line = 0;
  size_t column = 0;
};

/// Represents a GraphQL error
struct GraphQLError {
  std::string message;
  std::vector<SourceLocation> locations;
  std::vector<std::variant<std::string, size_t>> path;
  std::unordered_map<std::string, std::string> extensions;

  /// Convert to JSON for GraphQL response
  nlohmann::json toJSON() const;
};

// Forward declarations for simple value types used by GraphQLProtocol
struct NullValue {
  bool operator==(const NullValue&) const = default;
};
struct ListValue;
struct ObjectValue;

/// Represents a GraphQL variable value
struct Value {
  enum class Type { Null, Int, Float, String, Boolean, Enum, List, Object };

  Type type = Type::Null;
  std::variant<std::monostate, int64_t, double, std::string, bool,
               std::vector<Value>,
               std::vector<std::pair<std::string, Value>>>
      data;

  /// Create a null value
  static Value makeNull();
  /// Create an integer value
  static Value makeInt(int64_t val);
  /// Create a float value
  static Value makeFloat(double val);
  /// Create a string value
  static Value makeString(std::string val);
  /// Create a boolean value
  static Value makeBool(bool val);
  /// Create an enum value
  static Value makeEnum(std::string val);
  /// Create a list value
  static Value makeList(std::vector<Value> vals);
  /// Create an object value
  static Value makeObject(std::vector<std::pair<std::string, Value>> fields);

  /// Check if this value is null
  bool isNull() const { return type == Type::Null; }

  /// Get as string (throws if not a string)
  const std::string& asString() const;
  /// Get as int (throws if not an int)
  int64_t asInt() const;
  /// Get as float (throws if not a float)
  double asFloat() const;
  /// Get as bool (throws if not a bool)
  bool asBool() const;

  /// Get as list (throws if not a list)
  const std::vector<Value>& asList() const;

  /// Get as object (throws if not an object)
  const std::vector<std::pair<std::string, Value>>& asObject() const;
};

/// Simple list value (used by GraphQLProtocol for JSON conversion)
struct ListValue {
  std::vector<Value> values;
};

/// Simple object value (used by GraphQLProtocol for JSON conversion)
struct ObjectValue {
  std::unordered_map<std::string, Value> fields;
};

/// Represents an argument passed to a field
struct Argument {
  std::string name;
  Value value;
  SourceLocation location;
};

/// Represents a directive on a field or fragment
struct Directive {
  std::string name;
  std::vector<Argument> arguments;
  SourceLocation location;
};

/// The type of GraphQL operation
enum class OperationType { Query, Mutation, Subscription };

/// Represents a variable definition
struct VariableDefinition {
  std::string name;
  std::string typeName;  // The type as a string (e.g., "String!", "[Int]")
  bool isNonNull = false;
  bool isList = false;
  std::optional<Value> defaultValue;
  SourceLocation location;
};

// ============================================================================
// Selection Types - use a recursive variant via unique_ptr for fields
// ============================================================================

struct Field;
struct FragmentSpread;
struct InlineFragment;

/// A selection can be a field, fragment spread, or inline fragment
/// Using shared_ptr to handle recursive type definition
using Selection = std::variant<std::shared_ptr<Field>,
                                std::shared_ptr<FragmentSpread>,
                                std::shared_ptr<InlineFragment>>;

/// Represents a fragment spread (...FragmentName)
struct FragmentSpread {
  std::string name;
  std::vector<Directive> directives;
  SourceLocation location;
};

/// Represents an inline fragment (... on Type { ... })
struct InlineFragment {
  std::optional<std::string> typeCondition;
  std::vector<Directive> directives;
  std::vector<Selection> selectionSet;
  SourceLocation location;
};

/// Represents a field selection
struct Field {
  std::optional<std::string> alias;
  std::string name;
  std::vector<Argument> arguments;
  std::vector<Directive> directives;
  std::vector<Selection> selectionSet;
  SourceLocation location;

  /// Get the response key (alias if present, otherwise name)
  std::string responseKey() const { return alias.value_or(name); }

  /// Find an argument by name
  const Argument* findArgument(std::string_view argName) const;
};

/// Represents an operation (query, mutation, or subscription)
struct Operation {
  OperationType type = OperationType::Query;
  std::optional<std::string> name;
  std::vector<VariableDefinition> variableDefinitions;
  std::vector<Directive> directives;
  std::vector<Selection> selectionSet;
  SourceLocation location;
};

/// Represents a fragment definition
struct FragmentDefinition {
  std::string name;
  std::string typeCondition;
  std::vector<Directive> directives;
  std::vector<Selection> selectionSet;
  SourceLocation location;
};

/// Represents a complete GraphQL document
struct Document {
  std::vector<Operation> operations;
  std::vector<FragmentDefinition> fragments;

  /// Find an operation by name (or the single anonymous operation)
  const Operation* findOperation(
      std::optional<std::string_view> operationName = std::nullopt) const;

  /// Find a fragment by name
  const FragmentDefinition* findFragment(std::string_view name) const;
};

// ============================================================================
// GraphQL Parser
// ============================================================================

/// Parse result: either a Document or errors
using ParseResult = std::variant<Document, std::vector<GraphQLError>>;

/// Namespace for parser internal types (forward declared)
namespace peg {
struct ast;
}

/// Default security limits
constexpr size_t DEFAULT_QUERY_SIZE_LIMIT = 100 * 1024;  // 100 KB
constexpr size_t DEFAULT_DEPTH_LIMIT = 25;

/// The GraphQL parser
class GraphQLParser {
 public:
  /// Parse a GraphQL query string
  /// @param query The GraphQL query text
  /// @param depthLimit Maximum nesting depth (0 = use default of 25)
  /// @param querySizeLimit Maximum query size in bytes (0 = use default of 100KB)
  /// @return Either a parsed Document or a list of errors
  static ParseResult parse(std::string_view query, size_t depthLimit = 0,
                           size_t querySizeLimit = 0);

  /// Parse a GraphQL query with variables
  /// @param query The GraphQL query text
  /// @param variables JSON object containing variable values
  /// @param operationName Optional operation name to execute
  /// @return Either a parsed Document or a list of errors
  static ParseResult parseWithVariables(
      std::string_view query, const nlohmann::json& variables,
      std::optional<std::string_view> operationName = std::nullopt);

 private:
  /// Convert AST to our Document type
  static Document convertAST(const peg::ast& ast);

  /// Validate the parsed document
  static std::vector<GraphQLError> validate(const Document& doc);
};

}  // namespace graphql

#endif  // QLEVER_SRC_PARSER_GRAPHQL_GRAPHQLPARSER_H
