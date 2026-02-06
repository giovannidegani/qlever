// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifndef QLEVER_SRC_ENGINE_GRAPHQL_MUTATIONSCHEMABUILDER_H
#define QLEVER_SRC_ENGINE_GRAPHQL_MUTATIONSCHEMABUILDER_H

#include <string>
#include <vector>

#include "parser/graphql/GraphQLSchema.h"

namespace graphql {

/// Configuration for mutation schema generation
struct MutationSchemaConfig {
  /// Enable create mutations (createPerson, etc.)
  bool enableCreate = true;

  /// Enable update mutations (updatePerson, etc.)
  bool enableUpdate = true;

  /// Enable delete mutations (deletePerson, etc.)
  bool enableDelete = true;

  /// Enable upsert mutations (upsertPerson, etc.)
  bool enableUpsert = true;

  /// Enable batch create mutations (createPersons, etc.)
  bool enableBatchCreate = true;

  /// Enable batch delete mutations (deletePersons, etc.)
  bool enableBatchDelete = true;

  /// Enable nested mutations (connect, disconnect, create, delete for relations)
  bool enableNestedMutations = true;

  /// Enable versioning (_version field for optimistic locking)
  bool enableVersioning = true;

  /// Enable timestamps (_createdAt, _updatedAt fields)
  bool enableTimestamps = true;

  /// IRI generation configuration
  IriConfig iriConfig;

  /// Prefix for mutation names (e.g., "" for createPerson, "mutation_" for mutation_createPerson)
  std::string mutationPrefix = "";

  /// Suffix for create input types (e.g., "Input" for CreatePersonInput)
  std::string createInputSuffix = "Input";

  /// Suffix for update input types (e.g., "Input" for UpdatePersonInput)
  std::string updateInputSuffix = "Input";
};

/// Builds mutation schema from existing GraphQL type schema
class MutationSchemaBuilder {
 public:
  /// Constructor
  /// @param config Configuration for mutation generation
  explicit MutationSchemaBuilder(MutationSchemaConfig config = {});

  /// Generate mutations and input types for a schema
  /// @param schema The schema to add mutations to (modified in place)
  void buildMutationSchema(GraphQLSchema& schema);

  /// Get the current configuration
  const MutationSchemaConfig& config() const { return config_; }

  /// Update the configuration
  void setConfig(MutationSchemaConfig config) { config_ = std::move(config); }

 private:
  MutationSchemaConfig config_;

  /// Generate input types for a schema type
  void generateInputTypes(const SchemaType& type, GraphQLSchema& schema);

  /// Generate CreateInput type for a schema type
  InputType generateCreateInputType(const SchemaType& type);

  /// Generate UpdateInput type for a schema type
  InputType generateUpdateInputType(const SchemaType& type);

  /// Generate RelationInput types for relation fields
  InputType generateRelationInputType(const SchemaType& type,
                                      const SchemaField& field);

  /// Generate mutation fields for a schema type
  void generateMutationFields(const SchemaType& type, GraphQLSchema& schema);

  /// Generate create mutation field
  MutationField generateCreateMutation(const SchemaType& type);

  /// Generate update mutation field
  MutationField generateUpdateMutation(const SchemaType& type);

  /// Generate delete mutation field
  MutationField generateDeleteMutation(const SchemaType& type);

  /// Generate upsert mutation field
  MutationField generateUpsertMutation(const SchemaType& type);

  /// Generate batch create mutation field
  MutationField generateBatchCreateMutation(const SchemaType& type);

  /// Generate batch delete mutation field
  MutationField generateBatchDeleteMutation(const SchemaType& type);

  /// Convert scalar type to input field type
  static InputField scalarToInputField(const SchemaField& field, bool required);

  /// Convert relation field to input field
  static InputField relationToInputField(const SchemaField& field, bool required);

  /// Generate camelCase name from type name
  static std::string toCamelCase(const std::string& typeName);

  /// Generate plural form of type name
  static std::string toPlural(const std::string& typeName);
};

}  // namespace graphql

#endif  // QLEVER_SRC_ENGINE_GRAPHQL_MUTATIONSCHEMABUILDER_H
