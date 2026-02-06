// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifndef QLEVER_SRC_PARSER_GRAPHQL_MUTATIONVALIDATOR_H
#define QLEVER_SRC_PARSER_GRAPHQL_MUTATIONVALIDATOR_H

#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "parser/graphql/GraphQLParser.h"
#include "parser/graphql/GraphQLSchema.h"
#include "util/json.h"

namespace graphql {

// ============================================================================
// Validation Error Types
// ============================================================================

/// Severity of validation error
enum class ValidationSeverity {
  Error,    // Must be fixed before mutation can proceed
  Warning   // Advisory, mutation can still proceed
};

/// A single validation error or warning
struct ValidationError {
  std::string message;
  std::string field;              // The field path where error occurred
  ValidationSeverity severity = ValidationSeverity::Error;
  std::string code;               // Machine-readable error code

  /// Convert to GraphQL error format
  GraphQLError toGraphQLError() const;
};

/// Result of validation
struct ValidationResult {
  bool valid = true;
  std::vector<ValidationError> errors;

  /// Add an error
  void addError(std::string message, std::string field = "",
                std::string code = "VALIDATION_ERROR");

  /// Add a warning
  void addWarning(std::string message, std::string field = "",
                  std::string code = "VALIDATION_WARNING");

  /// Merge another validation result into this one
  void merge(const ValidationResult& other);

  /// Convert all errors to GraphQL errors
  std::vector<GraphQLError> toGraphQLErrors() const;
};

// ============================================================================
// Validation Configuration
// ============================================================================

/// Configuration for mutation validation
struct ValidationConfig {
  /// Validate required fields are present for create mutations
  bool validateRequiredFields = true;

  /// Validate scalar types match schema
  bool validateScalarTypes = true;

  /// Validate IDs reference existing entities (requires DB query)
  bool validateReferences = false;

  /// Validate OWL cardinality constraints
  bool validateCardinality = false;

  /// Validate that relation targets match expected types
  bool validateRelationTypes = true;

  /// Maximum string length (0 = no limit)
  size_t maxStringLength = 0;

  /// Maximum list size (0 = no limit)
  size_t maxListSize = 0;

  /// Custom validation function for IDs
  std::function<bool(const std::string&)> validateIdExists;
};

// ============================================================================
// Mutation Validator
// ============================================================================

/// Validates GraphQL mutation inputs against the schema
class MutationValidator {
 public:
  /// Constructor
  /// @param schema The GraphQL schema with type definitions
  /// @param config Validation configuration
  explicit MutationValidator(const GraphQLSchema& schema,
                             ValidationConfig config = {});

  /// Validate a create mutation input
  /// @param typeName The GraphQL type being created
  /// @param input The input data as JSON
  /// @return Validation result with any errors
  ValidationResult validateCreateInput(const std::string& typeName,
                                       const nlohmann::json& input);

  /// Validate an update mutation input
  /// @param typeName The GraphQL type being updated
  /// @param input The input data as JSON
  /// @return Validation result with any errors
  ValidationResult validateUpdateInput(const std::string& typeName,
                                       const nlohmann::json& input);

  /// Validate a delete mutation (just validates the ID)
  /// @param typeName The GraphQL type being deleted
  /// @param id The ID of the entity to delete
  /// @return Validation result with any errors
  ValidationResult validateDeleteInput(const std::string& typeName,
                                       const std::string& id);

  /// Validate a batch create mutation input
  /// @param typeName The GraphQL type being created
  /// @param inputs Array of input data
  /// @return Validation result with any errors
  ValidationResult validateBatchCreateInput(const std::string& typeName,
                                            const nlohmann::json& inputs);

  /// Validate a batch delete mutation input
  /// @param typeName The GraphQL type being deleted
  /// @param ids Array of IDs to delete
  /// @return Validation result with any errors
  ValidationResult validateBatchDeleteInput(const std::string& typeName,
                                            const nlohmann::json& ids);

  /// Get the current configuration
  const ValidationConfig& config() const { return config_; }

  /// Update the configuration
  void setConfig(ValidationConfig config) { config_ = std::move(config); }

 private:
  const GraphQLSchema& schema_;
  ValidationConfig config_;

  /// Validate a single field value against its schema definition
  void validateFieldValue(const std::string& fieldPath,
                          const SchemaField& fieldDef,
                          const nlohmann::json& value,
                          ValidationResult& result);

  /// Validate a scalar value against its expected type
  void validateScalarValue(const std::string& fieldPath,
                           ScalarType expectedType,
                           const nlohmann::json& value,
                           ValidationResult& result);

  /// Validate a relation value (ID reference)
  void validateRelationValue(const std::string& fieldPath,
                             const SchemaField& fieldDef,
                             const nlohmann::json& value,
                             ValidationResult& result);

  /// Validate string constraints (length, format, etc.)
  void validateStringConstraints(const std::string& fieldPath,
                                 const std::string& value,
                                 ValidationResult& result);

  /// Validate list constraints (size, element types)
  void validateListConstraints(const std::string& fieldPath,
                               const nlohmann::json& value,
                               const SchemaField& fieldDef,
                               ValidationResult& result);

  /// Check if a type exists in the schema
  bool typeExists(const std::string& typeName) const;

  /// Check if a field exists on a type
  bool fieldExists(const std::string& typeName,
                   const std::string& fieldName) const;

  /// Get all required fields for a type
  std::vector<std::string> getRequiredFields(const SchemaType& type) const;
};

}  // namespace graphql

#endif  // QLEVER_SRC_PARSER_GRAPHQL_MUTATIONVALIDATOR_H
