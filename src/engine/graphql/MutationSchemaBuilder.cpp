// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#include "engine/graphql/MutationSchemaBuilder.h"

#include <algorithm>
#include <cctype>

namespace graphql {

// ============================================================================
// Constructor
// ============================================================================

MutationSchemaBuilder::MutationSchemaBuilder(MutationSchemaConfig config)
    : config_(std::move(config)) {}

// ============================================================================
// Main Schema Building
// ============================================================================

void MutationSchemaBuilder::buildMutationSchema(GraphQLSchema& schema) {
  // Set schema-level configuration
  schema.setIriConfig(config_.iriConfig);
  schema.setVersioningEnabled(config_.enableVersioning);
  schema.setTimestampsEnabled(config_.enableTimestamps);

  // Generate input types and mutations for each type
  for (const auto& type : schema.getTypes()) {
    // Skip interface types for now (they don't get direct mutations)
    if (type.isInterface) {
      continue;
    }

    // Generate input types
    generateInputTypes(type, schema);

    // Generate mutation fields
    generateMutationFields(type, schema);
  }
}

// ============================================================================
// Input Type Generation
// ============================================================================

void MutationSchemaBuilder::generateInputTypes(const SchemaType& type,
                                                GraphQLSchema& schema) {
  // Generate CreateInput type
  if (config_.enableCreate || config_.enableBatchCreate || config_.enableUpsert) {
    schema.addInputType(generateCreateInputType(type));
  }

  // Generate UpdateInput type
  if (config_.enableUpdate || config_.enableUpsert) {
    schema.addInputType(generateUpdateInputType(type));
  }

  // Generate RelationInput types for relation fields
  if (config_.enableNestedMutations) {
    for (const auto& field : type.fields) {
      if (field.isObjectProperty) {
        schema.addInputType(generateRelationInputType(type, field));
      }
    }
  }
}

InputType MutationSchemaBuilder::generateCreateInputType(const SchemaType& type) {
  InputType inputType;
  inputType.name = "Create" + type.graphqlName + config_.createInputSuffix;
  inputType.forTypeName = type.graphqlName;
  inputType.isCreateInput = true;

  // Add all fields from the type (required for non-nullable fields)
  for (const auto& field : type.fields) {
    if (field.isObjectProperty) {
      // Relation field - use relation input type for nested mutations
      if (config_.enableNestedMutations) {
        InputField inputField;
        inputField.name = field.graphqlName;
        inputField.objectTypeName = type.graphqlName +
            toCamelCase(field.graphqlName) + "RelationInput";
        inputField.isRelation = true;
        inputField.isList = false;  // The relation input handles list vs single
        inputField.isRequired = false;  // Relations are optional in create
        inputType.fields.push_back(std::move(inputField));
      } else {
        // Simple relation - just IDs
        InputField inputField;
        inputField.name = field.graphqlName;
        inputField.scalarType = ScalarType::ID;
        inputField.isRelation = true;
        inputField.isList = field.isList;
        inputField.isRequired = false;
        inputType.fields.push_back(std::move(inputField));
      }
    } else {
      // Scalar field
      inputType.fields.push_back(scalarToInputField(field, field.isRequired));
    }
  }

  return inputType;
}

InputType MutationSchemaBuilder::generateUpdateInputType(const SchemaType& type) {
  InputType inputType;
  inputType.name = "Update" + type.graphqlName + config_.updateInputSuffix;
  inputType.forTypeName = type.graphqlName;
  inputType.isUpdateInput = true;

  // Add all fields from the type (all optional for updates)
  for (const auto& field : type.fields) {
    if (field.isObjectProperty) {
      // Relation field
      if (config_.enableNestedMutations) {
        InputField inputField;
        inputField.name = field.graphqlName;
        inputField.objectTypeName = type.graphqlName +
            toCamelCase(field.graphqlName) + "RelationUpdateInput";
        inputField.isRelation = true;
        inputField.isList = false;
        inputField.isRequired = false;
        inputType.fields.push_back(std::move(inputField));
      } else {
        InputField inputField;
        inputField.name = field.graphqlName;
        inputField.scalarType = ScalarType::ID;
        inputField.isRelation = true;
        inputField.isList = field.isList;
        inputField.isRequired = false;
        inputType.fields.push_back(std::move(inputField));
      }
    } else {
      // Scalar field - all optional for updates
      inputType.fields.push_back(scalarToInputField(field, false));
    }
  }

  return inputType;
}

InputType MutationSchemaBuilder::generateRelationInputType(
    const SchemaType& type, const SchemaField& field) {
  InputType inputType;
  inputType.name = type.graphqlName + toCamelCase(field.graphqlName) +
                   "RelationInput";
  inputType.forTypeName = field.objectTypeName;
  inputType.isRelationInput = true;

  // connect: [ID!] - Connect to existing entities
  {
    InputField connectField;
    connectField.name = "connect";
    connectField.scalarType = ScalarType::ID;
    connectField.isList = true;
    connectField.isRequired = false;
    inputType.fields.push_back(std::move(connectField));
  }

  // create: [CreateXInput!] - Create new entities and connect
  {
    InputField createField;
    createField.name = "create";
    createField.objectTypeName = "Create" + field.objectTypeName +
                                  config_.createInputSuffix;
    createField.isRelation = true;
    createField.isList = true;
    createField.isRequired = false;
    inputType.fields.push_back(std::move(createField));
  }

  return inputType;
}

// ============================================================================
// Mutation Field Generation
// ============================================================================

void MutationSchemaBuilder::generateMutationFields(const SchemaType& type,
                                                    GraphQLSchema& schema) {
  if (config_.enableCreate) {
    schema.addMutationField(generateCreateMutation(type));
  }

  if (config_.enableUpdate) {
    schema.addMutationField(generateUpdateMutation(type));
  }

  if (config_.enableDelete) {
    schema.addMutationField(generateDeleteMutation(type));
  }

  if (config_.enableUpsert) {
    schema.addMutationField(generateUpsertMutation(type));
  }

  if (config_.enableBatchCreate) {
    schema.addMutationField(generateBatchCreateMutation(type));
  }

  if (config_.enableBatchDelete) {
    schema.addMutationField(generateBatchDeleteMutation(type));
  }
}

MutationField MutationSchemaBuilder::generateCreateMutation(
    const SchemaType& type) {
  std::string name = config_.mutationPrefix + "create" + type.graphqlName;
  std::string inputType = "Create" + type.graphqlName + config_.createInputSuffix;

  return MutationField{
      std::move(name),
      MutationType::Create,
      type.graphqlName,
      std::move(inputType),
      type.graphqlName,  // Returns the created entity
      false              // Single entity, not list
  };
}

MutationField MutationSchemaBuilder::generateUpdateMutation(
    const SchemaType& type) {
  std::string name = config_.mutationPrefix + "update" + type.graphqlName;
  std::string inputType = "Update" + type.graphqlName + config_.updateInputSuffix;

  return MutationField{
      std::move(name),
      MutationType::Update,
      type.graphqlName,
      std::move(inputType),
      type.graphqlName,  // Returns the updated entity (or null if not found)
      false
  };
}

MutationField MutationSchemaBuilder::generateDeleteMutation(
    const SchemaType& type) {
  std::string name = config_.mutationPrefix + "delete" + type.graphqlName;

  return MutationField{
      std::move(name),
      MutationType::Delete,
      type.graphqlName,
      "",                // No input type, just ID argument
      "DeleteResult",
      false
  };
}

MutationField MutationSchemaBuilder::generateUpsertMutation(
    const SchemaType& type) {
  std::string name = config_.mutationPrefix + "upsert" + type.graphqlName;
  std::string createInput = "Create" + type.graphqlName + config_.createInputSuffix;
  std::string updateInput = "Update" + type.graphqlName + config_.updateInputSuffix;

  // For upsert, we combine create and update inputs
  // The actual input handling is more complex - uses where, create, update args
  return MutationField{
      std::move(name),
      MutationType::Upsert,
      type.graphqlName,
      std::move(createInput),  // Primary input type (create)
      type.graphqlName,
      false
  };
}

MutationField MutationSchemaBuilder::generateBatchCreateMutation(
    const SchemaType& type) {
  std::string name = config_.mutationPrefix + "create" + toPlural(type.graphqlName);
  std::string inputType = "Create" + type.graphqlName + config_.createInputSuffix;

  return MutationField{
      std::move(name),
      MutationType::BatchCreate,
      type.graphqlName,
      std::move(inputType),
      type.graphqlName,  // Returns list of created entities
      true               // Returns a list
  };
}

MutationField MutationSchemaBuilder::generateBatchDeleteMutation(
    const SchemaType& type) {
  std::string name = config_.mutationPrefix + "delete" + toPlural(type.graphqlName);

  return MutationField{
      std::move(name),
      MutationType::BatchDelete,
      type.graphqlName,
      "",                   // No input type, just IDs argument
      "BatchDeleteResult",
      false
  };
}

// ============================================================================
// Helper Methods
// ============================================================================

InputField MutationSchemaBuilder::scalarToInputField(const SchemaField& field,
                                                      bool required) {
  InputField inputField;
  inputField.name = field.graphqlName;
  inputField.scalarType = field.scalarType;
  inputField.isList = field.isList;
  inputField.isRequired = required;
  inputField.isRelation = false;
  return inputField;
}

InputField MutationSchemaBuilder::relationToInputField(const SchemaField& field,
                                                        bool required) {
  InputField inputField;
  inputField.name = field.graphqlName;
  inputField.objectTypeName = field.objectTypeName;
  inputField.isList = field.isList;
  inputField.isRequired = required;
  inputField.isRelation = true;
  return inputField;
}

std::string MutationSchemaBuilder::toCamelCase(const std::string& typeName) {
  if (typeName.empty()) return typeName;

  std::string result = typeName;
  result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
  return result;
}

std::string MutationSchemaBuilder::toPlural(const std::string& typeName) {
  if (typeName.empty()) return typeName;

  // Simple English pluralization rules
  char lastChar = typeName.back();

  if (lastChar == 's' || lastChar == 'x' || lastChar == 'z') {
    return typeName + "es";
  }
  if (lastChar == 'y' && typeName.size() > 1) {
    char beforeLast = typeName[typeName.size() - 2];
    // Check if preceded by consonant
    if (beforeLast != 'a' && beforeLast != 'e' && beforeLast != 'i' &&
        beforeLast != 'o' && beforeLast != 'u') {
      return typeName.substr(0, typeName.size() - 1) + "ies";
    }
  }
  if (typeName.size() >= 2) {
    std::string lastTwo = typeName.substr(typeName.size() - 2);
    if (lastTwo == "ch" || lastTwo == "sh") {
      return typeName + "es";
    }
  }

  return typeName + "s";
}

}  // namespace graphql
