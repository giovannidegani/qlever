// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#include "parser/graphql/MutationValidator.h"

#include <regex>
#include <sstream>

namespace graphql {

// ============================================================================
// ValidationError Implementation
// ============================================================================

GraphQLError ValidationError::toGraphQLError() const {
  GraphQLError err;
  err.message = message;
  if (!field.empty()) {
    err.path.push_back(field);
  }
  err.extensions["code"] = code;
  if (severity == ValidationSeverity::Warning) {
    err.extensions["severity"] = "warning";
  }
  return err;
}

// ============================================================================
// ValidationResult Implementation
// ============================================================================

void ValidationResult::addError(std::string message, std::string field,
                                std::string code) {
  valid = false;
  errors.push_back(ValidationError{
      std::move(message),
      std::move(field),
      ValidationSeverity::Error,
      std::move(code)});
}

void ValidationResult::addWarning(std::string message, std::string field,
                                  std::string code) {
  errors.push_back(ValidationError{
      std::move(message),
      std::move(field),
      ValidationSeverity::Warning,
      std::move(code)});
}

void ValidationResult::merge(const ValidationResult& other) {
  if (!other.valid) {
    valid = false;
  }
  errors.insert(errors.end(), other.errors.begin(), other.errors.end());
}

std::vector<GraphQLError> ValidationResult::toGraphQLErrors() const {
  std::vector<GraphQLError> result;
  result.reserve(errors.size());
  for (const auto& error : errors) {
    if (error.severity == ValidationSeverity::Error) {
      result.push_back(error.toGraphQLError());
    }
  }
  return result;
}

// ============================================================================
// MutationValidator Implementation
// ============================================================================

MutationValidator::MutationValidator(const GraphQLSchema& schema,
                                     ValidationConfig config)
    : schema_(schema), config_(std::move(config)) {}

ValidationResult MutationValidator::validateCreateInput(
    const std::string& typeName, const nlohmann::json& input) {
  ValidationResult result;

  // Check type exists
  const SchemaType* type = schema_.findType(typeName);
  if (!type) {
    result.addError("Unknown type: " + typeName, "", "UNKNOWN_TYPE");
    return result;
  }

  // Validate input is an object
  if (!input.is_object()) {
    result.addError("Input must be an object", "", "INVALID_INPUT_TYPE");
    return result;
  }

  // Check required fields for create
  if (config_.validateRequiredFields) {
    auto requiredFields = getRequiredFields(*type);
    for (const auto& fieldName : requiredFields) {
      if (!input.contains(fieldName) || input[fieldName].is_null()) {
        result.addError("Required field '" + fieldName + "' is missing",
                        fieldName, "REQUIRED_FIELD_MISSING");
      }
    }
  }

  // Validate each provided field
  for (const auto& [fieldName, value] : input.items()) {
    // Skip internal fields
    if (fieldName[0] == '_') {
      continue;
    }

    const SchemaField* fieldDef = type->findField(fieldName);
    if (!fieldDef) {
      result.addWarning("Unknown field '" + fieldName + "' will be ignored",
                        fieldName, "UNKNOWN_FIELD");
      continue;
    }

    validateFieldValue(fieldName, *fieldDef, value, result);
  }

  return result;
}

ValidationResult MutationValidator::validateUpdateInput(
    const std::string& typeName, const nlohmann::json& input) {
  ValidationResult result;

  // Check type exists
  const SchemaType* type = schema_.findType(typeName);
  if (!type) {
    result.addError("Unknown type: " + typeName, "", "UNKNOWN_TYPE");
    return result;
  }

  // Validate input is an object
  if (!input.is_object()) {
    result.addError("Input must be an object", "", "INVALID_INPUT_TYPE");
    return result;
  }

  // For updates, no fields are required (all optional)
  // But we still validate the fields that are provided

  // Check at least one field is provided
  bool hasFields = false;
  for (const auto& [fieldName, value] : input.items()) {
    if (fieldName[0] != '_' && !value.is_null()) {
      hasFields = true;
      break;
    }
  }

  if (!hasFields) {
    result.addWarning("Update input has no fields to update",
                      "", "EMPTY_UPDATE");
  }

  // Validate each provided field
  for (const auto& [fieldName, value] : input.items()) {
    // Skip internal fields and null values (null means "don't update")
    if (fieldName[0] == '_' || value.is_null()) {
      continue;
    }

    const SchemaField* fieldDef = type->findField(fieldName);
    if (!fieldDef) {
      result.addWarning("Unknown field '" + fieldName + "' will be ignored",
                        fieldName, "UNKNOWN_FIELD");
      continue;
    }

    validateFieldValue(fieldName, *fieldDef, value, result);
  }

  return result;
}

ValidationResult MutationValidator::validateDeleteInput(
    const std::string& typeName, const std::string& id) {
  ValidationResult result;

  // Check type exists
  const SchemaType* type = schema_.findType(typeName);
  if (!type) {
    result.addError("Unknown type: " + typeName, "", "UNKNOWN_TYPE");
    return result;
  }

  // Validate ID is not empty
  if (id.empty()) {
    result.addError("ID cannot be empty", "id", "INVALID_ID");
    return result;
  }

  // Validate ID exists in database (if configured)
  if (config_.validateReferences && config_.validateIdExists) {
    if (!config_.validateIdExists(id)) {
      result.addError("Entity with ID '" + id + "' not found",
                      "id", "ENTITY_NOT_FOUND");
    }
  }

  return result;
}

ValidationResult MutationValidator::validateBatchCreateInput(
    const std::string& typeName, const nlohmann::json& inputs) {
  ValidationResult result;

  // Validate inputs is an array
  if (!inputs.is_array()) {
    result.addError("Batch create input must be an array",
                    "", "INVALID_INPUT_TYPE");
    return result;
  }

  // Validate max batch size
  if (config_.maxListSize > 0 && inputs.size() > config_.maxListSize) {
    result.addError("Batch size exceeds maximum of " +
                        std::to_string(config_.maxListSize),
                    "", "BATCH_SIZE_EXCEEDED");
    return result;
  }

  // Validate each item
  size_t index = 0;
  for (const auto& item : inputs) {
    auto itemResult = validateCreateInput(typeName, item);

    // Prefix field paths with array index
    for (auto& error : itemResult.errors) {
      error.field = "[" + std::to_string(index) + "]." + error.field;
    }

    result.merge(itemResult);
    index++;
  }

  return result;
}

ValidationResult MutationValidator::validateBatchDeleteInput(
    const std::string& typeName, const nlohmann::json& ids) {
  ValidationResult result;

  // Check type exists
  const SchemaType* type = schema_.findType(typeName);
  if (!type) {
    result.addError("Unknown type: " + typeName, "", "UNKNOWN_TYPE");
    return result;
  }

  // Validate ids is an array
  if (!ids.is_array()) {
    result.addError("Batch delete IDs must be an array",
                    "", "INVALID_INPUT_TYPE");
    return result;
  }

  // Validate max batch size
  if (config_.maxListSize > 0 && ids.size() > config_.maxListSize) {
    result.addError("Batch size exceeds maximum of " +
                        std::to_string(config_.maxListSize),
                    "", "BATCH_SIZE_EXCEEDED");
    return result;
  }

  // Validate each ID
  size_t index = 0;
  for (const auto& id : ids) {
    std::string fieldPath = "[" + std::to_string(index) + "]";

    if (!id.is_string()) {
      result.addError("ID must be a string", fieldPath, "INVALID_ID_TYPE");
    } else if (id.get<std::string>().empty()) {
      result.addError("ID cannot be empty", fieldPath, "INVALID_ID");
    } else if (config_.validateReferences && config_.validateIdExists) {
      std::string idStr = id.get<std::string>();
      if (!config_.validateIdExists(idStr)) {
        result.addWarning("Entity with ID '" + idStr + "' not found",
                          fieldPath, "ENTITY_NOT_FOUND");
      }
    }

    index++;
  }

  return result;
}

void MutationValidator::validateFieldValue(const std::string& fieldPath,
                                           const SchemaField& fieldDef,
                                           const nlohmann::json& value,
                                           ValidationResult& result) {
  // Handle null values
  if (value.is_null()) {
    if (fieldDef.isRequired) {
      result.addError("Field '" + fieldPath + "' cannot be null",
                      fieldPath, "NULL_NOT_ALLOWED");
    }
    return;
  }

  // Handle list fields
  if (fieldDef.isList) {
    if (!value.is_array()) {
      result.addError("Field '" + fieldPath + "' must be an array",
                      fieldPath, "EXPECTED_ARRAY");
      return;
    }

    validateListConstraints(fieldPath, value, fieldDef, result);

    // Validate each element
    size_t index = 0;
    for (const auto& element : value) {
      std::string elementPath = fieldPath + "[" + std::to_string(index) + "]";

      if (fieldDef.isObjectProperty) {
        validateRelationValue(elementPath, fieldDef, element, result);
      } else {
        validateScalarValue(elementPath, fieldDef.scalarType, element, result);
      }

      index++;
    }
  } else {
    // Single value field
    if (fieldDef.isObjectProperty) {
      validateRelationValue(fieldPath, fieldDef, value, result);
    } else {
      validateScalarValue(fieldPath, fieldDef.scalarType, value, result);
    }
  }
}

void MutationValidator::validateScalarValue(const std::string& fieldPath,
                                            ScalarType expectedType,
                                            const nlohmann::json& value,
                                            ValidationResult& result) {
  if (!config_.validateScalarTypes) {
    return;
  }

  bool valid = true;
  std::string expectedTypeName;

  switch (expectedType) {
    case ScalarType::STRING:
      expectedTypeName = "String";
      valid = value.is_string();
      if (valid) {
        validateStringConstraints(fieldPath, value.get<std::string>(), result);
      }
      break;

    case ScalarType::INT:
      expectedTypeName = "Int";
      valid = value.is_number_integer();
      break;

    case ScalarType::FLOAT:
      expectedTypeName = "Float";
      valid = value.is_number();
      break;

    case ScalarType::BOOLEAN:
      expectedTypeName = "Boolean";
      valid = value.is_boolean();
      break;

    case ScalarType::ID:
      expectedTypeName = "ID";
      valid = value.is_string();
      break;

    case ScalarType::DATETIME:
      expectedTypeName = "DateTime";
      if (!value.is_string()) {
        valid = false;
      } else {
        // Basic ISO 8601 validation
        std::string str = value.get<std::string>();
        std::regex dateTimeRegex(
            R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(Z|[+-]\d{2}:\d{2})?)");
        valid = std::regex_match(str, dateTimeRegex);
      }
      break;

    case ScalarType::DATE:
      expectedTypeName = "Date";
      if (!value.is_string()) {
        valid = false;
      } else {
        std::string str = value.get<std::string>();
        std::regex dateRegex(R"(\d{4}-\d{2}-\d{2})");
        valid = std::regex_match(str, dateRegex);
      }
      break;

    case ScalarType::TIME:
      expectedTypeName = "Time";
      if (!value.is_string()) {
        valid = false;
      } else {
        std::string str = value.get<std::string>();
        std::regex timeRegex(R"(\d{2}:\d{2}:\d{2}(Z|[+-]\d{2}:\d{2})?)");
        valid = std::regex_match(str, timeRegex);
      }
      break;
  }

  if (!valid) {
    result.addError("Field '" + fieldPath + "' expects type " + expectedTypeName,
                    fieldPath, "INVALID_TYPE");
  }
}

void MutationValidator::validateRelationValue(const std::string& fieldPath,
                                              const SchemaField& fieldDef,
                                              const nlohmann::json& value,
                                              ValidationResult& result) {
  // Relation value can be:
  // 1. A string ID
  // 2. A nested relation input object { connect: [...], create: [...], ... }

  if (value.is_string()) {
    // Simple ID reference
    std::string id = value.get<std::string>();
    if (id.empty()) {
      result.addError("Relation ID cannot be empty", fieldPath, "INVALID_ID");
      return;
    }

    // Validate reference exists
    if (config_.validateReferences && config_.validateIdExists) {
      if (!config_.validateIdExists(id)) {
        result.addError("Referenced entity '" + id + "' not found",
                        fieldPath, "REFERENCE_NOT_FOUND");
      }
    }
  } else if (value.is_object()) {
    // Nested relation input
    // Validate each operation type
    if (value.contains("connect")) {
      if (!value["connect"].is_array()) {
        result.addError("'connect' must be an array",
                        fieldPath + ".connect", "INVALID_TYPE");
      } else {
        for (size_t i = 0; i < value["connect"].size(); i++) {
          const auto& id = value["connect"][i];
          if (!id.is_string()) {
            result.addError("ID must be a string",
                            fieldPath + ".connect[" + std::to_string(i) + "]",
                            "INVALID_ID_TYPE");
          } else if (config_.validateReferences && config_.validateIdExists) {
            if (!config_.validateIdExists(id.get<std::string>())) {
              result.addError("Referenced entity not found",
                              fieldPath + ".connect[" + std::to_string(i) + "]",
                              "REFERENCE_NOT_FOUND");
            }
          }
        }
      }
    }

    if (value.contains("create")) {
      if (!value["create"].is_array()) {
        result.addError("'create' must be an array",
                        fieldPath + ".create", "INVALID_TYPE");
      } else {
        // Recursively validate create inputs
        for (size_t i = 0; i < value["create"].size(); i++) {
          auto createResult =
              validateCreateInput(fieldDef.objectTypeName, value["create"][i]);
          for (auto& error : createResult.errors) {
            error.field =
                fieldPath + ".create[" + std::to_string(i) + "]." + error.field;
          }
          result.merge(createResult);
        }
      }
    }

    if (value.contains("disconnect")) {
      if (!value["disconnect"].is_array()) {
        result.addError("'disconnect' must be an array",
                        fieldPath + ".disconnect", "INVALID_TYPE");
      }
    }

    if (value.contains("delete")) {
      if (!value["delete"].is_array()) {
        result.addError("'delete' must be an array",
                        fieldPath + ".delete", "INVALID_TYPE");
      }
    }

    if (value.contains("set")) {
      if (!value["set"].is_array()) {
        result.addError("'set' must be an array",
                        fieldPath + ".set", "INVALID_TYPE");
      }
    }
  } else {
    result.addError("Relation value must be an ID or relation input object",
                    fieldPath, "INVALID_RELATION_VALUE");
  }
}

void MutationValidator::validateStringConstraints(const std::string& fieldPath,
                                                  const std::string& value,
                                                  ValidationResult& result) {
  if (config_.maxStringLength > 0 && value.length() > config_.maxStringLength) {
    result.addError("String exceeds maximum length of " +
                        std::to_string(config_.maxStringLength),
                    fieldPath, "STRING_TOO_LONG");
  }
}

void MutationValidator::validateListConstraints(const std::string& fieldPath,
                                                const nlohmann::json& value,
                                                const SchemaField& fieldDef,
                                                ValidationResult& result) {
  if (config_.maxListSize > 0 && value.size() > config_.maxListSize) {
    result.addError("List exceeds maximum size of " +
                        std::to_string(config_.maxListSize),
                    fieldPath, "LIST_TOO_LARGE");
  }
}

bool MutationValidator::typeExists(const std::string& typeName) const {
  return schema_.findType(typeName) != nullptr;
}

bool MutationValidator::fieldExists(const std::string& typeName,
                                    const std::string& fieldName) const {
  const SchemaType* type = schema_.findType(typeName);
  if (!type) {
    return false;
  }
  return type->findField(fieldName) != nullptr;
}

std::vector<std::string> MutationValidator::getRequiredFields(
    const SchemaType& type) const {
  std::vector<std::string> required;
  for (const auto& field : type.fields) {
    if (field.isRequired) {
      required.push_back(field.graphqlName);
    }
  }
  return required;
}

}  // namespace graphql
