// Copyright 2025, University of Freiburg,
//                 Chair of Algorithms and Data Structures.
// Author: GraphQL Support for QLever

#ifndef QLEVER_SRC_PARSER_GRAPHQL_GRAPHQLSCHEMA_H
#define QLEVER_SRC_PARSER_GRAPHQL_GRAPHQLSCHEMA_H

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "util/json.h"

namespace graphql {

// Represents an XSD datatype mapped to a GraphQL scalar
enum class ScalarType {
  STRING,
  INT,
  FLOAT,
  BOOLEAN,
  ID,
  DATETIME,
  DATE,
  TIME
};

// Convert ScalarType to its GraphQL SDL name
inline std::string scalarTypeToString(ScalarType type) {
  switch (type) {
    case ScalarType::STRING:
      return "String";
    case ScalarType::INT:
      return "Int";
    case ScalarType::FLOAT:
      return "Float";
    case ScalarType::BOOLEAN:
      return "Boolean";
    case ScalarType::ID:
      return "ID";
    case ScalarType::DATETIME:
      return "DateTime";
    case ScalarType::DATE:
      return "Date";
    case ScalarType::TIME:
      return "Time";
  }
  return "String";  // Default fallback
}

// Represents a field in a GraphQL type
struct SchemaField {
  std::string graphqlName;     // GraphQL field name
  std::string propertyIRI;     // The RDF property IRI
  ScalarType scalarType;       // For scalar fields
  std::string objectTypeName;  // For object fields (empty if scalar)
  bool isObjectProperty;       // true if range is another class
  bool isList;                 // RDF properties are typically multi-valued
  bool isRequired;             // Non-null in GraphQL
  std::optional<std::string> label;
  std::optional<std::string> comment;

  // Default constructor
  SchemaField() = default;

  // Constructor for scalar fields
  SchemaField(std::string graphqlName, std::string propertyIRI,
              ScalarType scalarType, bool isList = true, bool isRequired = false)
      : graphqlName(std::move(graphqlName)),
        propertyIRI(std::move(propertyIRI)),
        scalarType(scalarType),
        objectTypeName(),
        isObjectProperty(false),
        isList(isList),
        isRequired(isRequired),
        label(std::nullopt),
        comment(std::nullopt) {}

  // Constructor for object fields
  SchemaField(std::string graphqlName, std::string propertyIRI,
              std::string objectTypeName, bool isList = true,
              bool isRequired = false)
      : graphqlName(std::move(graphqlName)),
        propertyIRI(std::move(propertyIRI)),
        scalarType(ScalarType::ID),
        objectTypeName(std::move(objectTypeName)),
        isObjectProperty(true),
        isList(isList),
        isRequired(isRequired),
        label(std::nullopt),
        comment(std::nullopt) {}
};

// Represents a GraphQL type generated from an RDF class
struct SchemaType {
  std::string graphqlName;  // GraphQL type name
  std::string classIRI;     // The RDF class IRI
  std::vector<SchemaField> fields;
  std::vector<std::string> superTypes;  // rdfs:subClassOf
  std::vector<std::string> implementsInterfaces;  // GraphQL interfaces this type implements
  bool isInterface = false;  // true if this type is a GraphQL interface
  std::optional<std::string> label;
  std::optional<std::string> comment;

  // Default constructor
  SchemaType() = default;

  // Constructor
  SchemaType(std::string graphqlName, std::string classIRI)
      : graphqlName(std::move(graphqlName)),
        classIRI(std::move(classIRI)),
        fields(),
        superTypes(),
        implementsInterfaces(),
        isInterface(false),
        label(std::nullopt),
        comment(std::nullopt) {}

  // Find a field by GraphQL name
  const SchemaField* findField(std::string_view name) const {
    for (const auto& field : fields) {
      if (field.graphqlName == name) {
        return &field;
      }
    }
    return nullptr;
  }

  // Add a field to this type
  void addField(SchemaField field) { fields.push_back(std::move(field)); }
};

// Represents a filter input type field
struct FilterField {
  std::string fieldName;
  ScalarType scalarType;
  bool isObjectProperty;

  FilterField() = default;

  FilterField(std::string fieldName, ScalarType scalarType,
              bool isObjectProperty = false)
      : fieldName(std::move(fieldName)),
        scalarType(scalarType),
        isObjectProperty(isObjectProperty) {}
};

// ============================================================================
// Mutation Types
// ============================================================================

/// Type of mutation operation
enum class MutationType {
  Create,     // INSERT DATA
  Update,     // DELETE/INSERT WHERE
  Delete,     // DELETE WHERE
  Upsert,     // INSERT WHERE NOT EXISTS + UPDATE
  BatchCreate,
  BatchDelete
};

/// Operation types for relation mutations
enum class RelationOperation {
  Connect,    // Add connection to existing entity
  Disconnect, // Remove connection
  Create,     // Create new related entity and connect
  Delete,     // Delete related entity
  Set         // Replace all connections
};

/// Input field for mutations (part of input types)
struct InputField {
  std::string name;
  ScalarType scalarType = ScalarType::STRING;
  std::string objectTypeName;  // For relation fields
  bool isRequired = false;
  bool isList = false;
  bool isRelation = false;     // True if this is a relation to another type
  std::optional<std::string> defaultValue;

  InputField() = default;

  // Scalar field constructor
  InputField(std::string name, ScalarType type, bool required = false,
             bool list = false)
      : name(std::move(name)),
        scalarType(type),
        isRequired(required),
        isList(list),
        isRelation(false) {}

  // Relation field constructor
  InputField(std::string name, std::string relatedType, bool required = false,
             bool list = false)
      : name(std::move(name)),
        objectTypeName(std::move(relatedType)),
        isRequired(required),
        isList(list),
        isRelation(true) {}
};

/// Input type for mutations (CreatePersonInput, UpdatePersonInput, etc.)
struct InputType {
  std::string name;                    // e.g., "CreatePersonInput"
  std::string forTypeName;             // e.g., "Person"
  bool isCreateInput = false;          // True for Create inputs (required fields)
  bool isUpdateInput = false;          // True for Update inputs (optional fields)
  bool isRelationInput = false;        // True for relation operation inputs
  std::vector<InputField> fields;

  InputType() = default;

  InputType(std::string name, std::string forType)
      : name(std::move(name)), forTypeName(std::move(forType)) {}

  const InputField* findField(std::string_view fieldName) const {
    for (const auto& field : fields) {
      if (field.name == fieldName) {
        return &field;
      }
    }
    return nullptr;
  }
};

/// Relation input for nested mutations
struct RelationInput {
  RelationOperation operation;
  std::vector<std::string> ids;        // For connect/disconnect/set/delete
  nlohmann::json createInputs;         // For nested create operations (as JSON)
};

/// Mutation field in the Mutation type
struct MutationField {
  std::string name;                    // e.g., "createPerson"
  MutationType mutationType;
  std::string targetTypeName;          // e.g., "Person"
  std::string inputTypeName;           // e.g., "CreatePersonInput"
  std::string returnTypeName;          // e.g., "Person" or "DeleteResult"
  bool returnsList = false;            // True for batch operations

  MutationField() = default;

  MutationField(std::string name, MutationType type, std::string targetType,
                std::string inputType, std::string returnType,
                bool returnsList = false)
      : name(std::move(name)),
        mutationType(type),
        targetTypeName(std::move(targetType)),
        inputTypeName(std::move(inputType)),
        returnTypeName(std::move(returnType)),
        returnsList(returnsList) {}
};

/// Result of a delete operation
struct DeleteResult {
  bool success = false;
  std::string id;
  size_t deletedTripleCount = 0;
};

/// Result of a batch delete operation
struct BatchDeleteResult {
  bool success = false;
  size_t deletedCount = 0;
  std::vector<std::string> deletedIds;
};

/// Entity metadata for versioning and timestamps
struct EntityMetadata {
  std::optional<std::string> createdAt;
  std::optional<std::string> updatedAt;
  std::optional<int64_t> version;
};

/// IRI generation strategy
enum class IriStrategy {
  UUID,       // Generate UUID-based IRI (default)
  UserProvided, // User provides the ID
  Template    // Use template from ontology
};

/// Configuration for IRI generation
struct IriConfig {
  IriStrategy strategy = IriStrategy::UUID;
  std::string baseIri;                 // Base IRI prefix
  std::string templatePattern;         // For Template strategy: "{email}" etc.
};

// Complete GraphQL schema derived from RDF
class GraphQLSchema {
 public:
  GraphQLSchema() = default;

  // Add a type to the schema
  void addType(SchemaType type) {
    std::string name = type.graphqlName;
    std::string iri = type.classIRI;
    size_t index = types_.size();
    types_.push_back(std::move(type));
    typesByName_[std::move(name)] = index;
    typesByIRI_[std::move(iri)] = index;
  }

  // Find a type by GraphQL name
  const SchemaType* findType(std::string_view name) const {
    auto it = typesByName_.find(std::string(name));
    if (it != typesByName_.end()) {
      return &types_[it->second];
    }
    return nullptr;
  }

  // Find a type by GraphQL name (mutable version)
  SchemaType* findTypeMutable(std::string_view name) {
    auto it = typesByName_.find(std::string(name));
    if (it != typesByName_.end()) {
      return &types_[it->second];
    }
    return nullptr;
  }

  // Find a type by RDF class IRI
  const SchemaType* findTypeByIRI(std::string_view iri) const {
    auto it = typesByIRI_.find(std::string(iri));
    if (it != typesByIRI_.end()) {
      return &types_[it->second];
    }
    return nullptr;
  }

  // Get all types
  const std::vector<SchemaType>& getTypes() const { return types_; }

  // Check if schema is empty
  bool empty() const { return types_.empty(); }

  // Get the number of types
  size_t size() const { return types_.size(); }

  // ============================================================================
  // Mutation Schema Management
  // ============================================================================

  // Add a mutation field to the schema
  void addMutationField(MutationField field) {
    std::string name = field.name;
    size_t index = mutationFields_.size();
    mutationFields_.push_back(std::move(field));
    mutationsByName_[std::move(name)] = index;
  }

  // Find a mutation field by name
  const MutationField* findMutation(std::string_view name) const {
    auto it = mutationsByName_.find(std::string(name));
    if (it != mutationsByName_.end()) {
      return &mutationFields_[it->second];
    }
    return nullptr;
  }

  // Get all mutation fields
  const std::vector<MutationField>& getMutationFields() const {
    return mutationFields_;
  }

  // Add an input type to the schema
  void addInputType(InputType inputType) {
    std::string name = inputType.name;
    size_t index = inputTypes_.size();
    inputTypes_.push_back(std::move(inputType));
    inputTypesByName_[std::move(name)] = index;
  }

  // Find an input type by name
  const InputType* findInputType(std::string_view name) const {
    auto it = inputTypesByName_.find(std::string(name));
    if (it != inputTypesByName_.end()) {
      return &inputTypes_[it->second];
    }
    return nullptr;
  }

  // Get all input types
  const std::vector<InputType>& getInputTypes() const { return inputTypes_; }

  // Check if mutations are enabled
  bool hasMutations() const { return !mutationFields_.empty(); }

  // Set IRI configuration for mutations
  void setIriConfig(IriConfig config) { iriConfig_ = std::move(config); }
  const IriConfig& getIriConfig() const { return iriConfig_; }

  // Enable/disable versioning for mutations
  void setVersioningEnabled(bool enabled) { versioningEnabled_ = enabled; }
  bool isVersioningEnabled() const { return versioningEnabled_; }

  // Enable/disable timestamp tracking
  void setTimestampsEnabled(bool enabled) { timestampsEnabled_ = enabled; }
  bool isTimestampsEnabled() const { return timestampsEnabled_; }

  // Convert XSD datatype IRI to ScalarType
  static ScalarType xsdToScalarType(std::string_view xsdType) {
    // Common XSD namespace prefix
    constexpr std::string_view XSD_PREFIX =
        "http://www.w3.org/2001/XMLSchema#";

    std::string_view localName = xsdType;
    if (xsdType.starts_with(XSD_PREFIX)) {
      localName = xsdType.substr(XSD_PREFIX.size());
    }

    if (localName == "string" || localName == "normalizedString" ||
        localName == "token" || localName == "language" ||
        localName == "Name" || localName == "NCName" ||
        localName == "NMTOKEN" || localName == "anyURI") {
      return ScalarType::STRING;
    }
    if (localName == "integer" || localName == "int" || localName == "long" ||
        localName == "short" || localName == "byte" ||
        localName == "nonNegativeInteger" ||
        localName == "nonPositiveInteger" || localName == "positiveInteger" ||
        localName == "negativeInteger" || localName == "unsignedLong" ||
        localName == "unsignedInt" || localName == "unsignedShort" ||
        localName == "unsignedByte") {
      return ScalarType::INT;
    }
    if (localName == "decimal" || localName == "float" ||
        localName == "double") {
      return ScalarType::FLOAT;
    }
    if (localName == "boolean") {
      return ScalarType::BOOLEAN;
    }
    if (localName == "dateTime" || localName == "dateTimeStamp") {
      return ScalarType::DATETIME;
    }
    if (localName == "date" || localName == "gYear" ||
        localName == "gYearMonth" || localName == "gMonth" ||
        localName == "gMonthDay" || localName == "gDay") {
      return ScalarType::DATE;
    }
    if (localName == "time") {
      return ScalarType::TIME;
    }

    // Default to STRING for unknown types
    return ScalarType::STRING;
  }

  // Convert IRI to valid GraphQL name
  // GraphQL names must match: /[_A-Za-z][_0-9A-Za-z]*/
  static std::string iriToGraphQLName(std::string_view iri) {
    // Extract local name from IRI
    std::string_view localName = iri;

    // Try to find fragment identifier (#)
    size_t hashPos = iri.rfind('#');
    if (hashPos != std::string_view::npos && hashPos + 1 < iri.size()) {
      localName = iri.substr(hashPos + 1);
    } else {
      // Try to find last path segment (/)
      size_t slashPos = iri.rfind('/');
      if (slashPos != std::string_view::npos && slashPos + 1 < iri.size()) {
        localName = iri.substr(slashPos + 1);
      }
    }

    // Convert to valid GraphQL name
    std::string result;
    result.reserve(localName.size());

    bool firstChar = true;
    for (char c : localName) {
      if (firstChar) {
        // First character must be letter or underscore
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
          result.push_back(c);
          firstChar = false;
        } else if (c >= '0' && c <= '9') {
          // Prefix with underscore if starts with digit
          result.push_back('_');
          result.push_back(c);
          firstChar = false;
        }
        // Skip invalid first characters
      } else {
        // Subsequent characters can be letters, digits, or underscore
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
          result.push_back(c);
        }
        // Skip other characters (e.g., hyphens, dots)
      }
    }

    // Ensure we have at least one character
    if (result.empty()) {
      result = "_Unknown";
    }

    return result;
  }

  // Generate the full GraphQL SDL (Schema Definition Language)
  std::string toSDL() const {
    std::string sdl;

    // Add custom scalar types
    sdl += "# Custom scalar types for RDF datatypes\n";
    sdl += "scalar DateTime\n";
    sdl += "scalar Date\n";
    sdl += "scalar Time\n\n";

    // Generate Query type
    sdl += "type Query {\n";
    for (const auto& type : types_) {
      sdl += "  " + type.graphqlName + "(";
      sdl += "id: ID, ";
      sdl += "filter: " + type.graphqlName + "Filter, ";
      sdl += "first: Int, ";
      sdl += "after: String, ";
      sdl += "orderBy: " + type.graphqlName + "OrderBy";
      sdl += "): [" + type.graphqlName + "!]!\n";
    }
    sdl += "\n  # Introspection\n";
    sdl += "  __schema: __Schema!\n";
    sdl += "  __type(name: String!): __Type\n";
    sdl += "}\n\n";

    // Generate interface definitions first
    for (const auto& type : types_) {
      if (!type.isInterface) continue;

      if (type.comment.has_value()) {
        sdl += "# " + type.comment.value() + "\n";
      }

      sdl += "interface " + type.graphqlName + " {\n";
      sdl += "  id: ID!\n";
      for (const auto& field : type.fields) {
        sdl += "  " + field.graphqlName + ": ";
        std::string typeName = field.isObjectProperty
                                   ? field.objectTypeName
                                   : scalarTypeToString(field.scalarType);
        if (field.isList) {
          sdl += "[" + typeName + (field.isRequired ? "!]!" : "]");
        } else {
          sdl += typeName + (field.isRequired ? "!" : "");
        }
        sdl += "\n";
      }
      sdl += "}\n\n";
    }

    // Generate types (non-interface)
    for (const auto& type : types_) {
      if (type.isInterface) continue;  // Skip interfaces, already generated

      // Add type comment if available
      if (type.comment.has_value()) {
        sdl += "# " + type.comment.value() + "\n";
      }

      sdl += "type " + type.graphqlName;

      // Add implements clause for interfaces
      if (!type.implementsInterfaces.empty()) {
        sdl += " implements ";
        for (size_t i = 0; i < type.implementsInterfaces.size(); ++i) {
          if (i > 0) sdl += " & ";
          sdl += type.implementsInterfaces[i];
        }
      }

      sdl += " {\n";

      // Always include id field
      sdl += "  id: ID!\n";

      // Add fields
      for (const auto& field : type.fields) {
        sdl += "  " + field.graphqlName;

        // Add language argument for string fields
        if (field.scalarType == ScalarType::STRING && !field.isObjectProperty) {
          sdl += "(lang: String)";
        }

        sdl += ": ";

        std::string typeName;
        if (field.isObjectProperty) {
          typeName = field.objectTypeName;
        } else {
          typeName = scalarTypeToString(field.scalarType);
        }

        if (field.isList) {
          sdl += "[" + typeName;
          if (field.isRequired) sdl += "!";
          sdl += "]";
        } else {
          sdl += typeName;
        }

        if (field.isRequired) sdl += "!";
        sdl += "\n";
      }

      sdl += "}\n\n";

      // Generate filter input type
      sdl += "input " + type.graphqlName + "Filter {\n";
      for (const auto& field : type.fields) {
        if (!field.isObjectProperty) {
          std::string filterTypeName;
          switch (field.scalarType) {
            case ScalarType::STRING:
              filterTypeName = "StringFilter";
              break;
            case ScalarType::INT:
              filterTypeName = "IntFilter";
              break;
            case ScalarType::FLOAT:
              filterTypeName = "FloatFilter";
              break;
            case ScalarType::BOOLEAN:
              filterTypeName = "Boolean";
              break;
            case ScalarType::DATETIME:
            case ScalarType::DATE:
              filterTypeName = "DateFilter";
              break;
            case ScalarType::TIME:
              filterTypeName = "TimeFilter";
              break;
            case ScalarType::ID:
              filterTypeName = "IDFilter";
              break;
          }
          sdl += "  " + field.graphqlName + ": " + filterTypeName + "\n";
        } else {
          // Object property filter
          sdl +=
              "  " + field.graphqlName + ": " + field.objectTypeName + "Filter\n";
        }
      }
      sdl += "  _and: [" + type.graphqlName + "Filter!]\n";
      sdl += "  _or: [" + type.graphqlName + "Filter!]\n";
      sdl += "  _not: " + type.graphqlName + "Filter\n";
      sdl += "}\n\n";

      // Generate order by enum
      sdl += "enum " + type.graphqlName + "OrderBy {\n";
      for (const auto& field : type.fields) {
        if (!field.isObjectProperty) {
          std::string upperName;
          for (char c : field.graphqlName) {
            if (c >= 'a' && c <= 'z') {
              upperName += static_cast<char>(c - 'a' + 'A');
            } else {
              upperName += c;
            }
          }
          sdl += "  " + upperName + "_ASC\n";
          sdl += "  " + upperName + "_DESC\n";
        }
      }
      sdl += "}\n\n";
    }

    // Add common filter types
    sdl += "# Common filter input types\n";
    sdl += "input StringFilter {\n";
    sdl += "  eq: String\n";
    sdl += "  ne: String\n";
    sdl += "  contains: String\n";
    sdl += "  startsWith: String\n";
    sdl += "  endsWith: String\n";
    sdl += "  in: [String!]\n";
    sdl += "  regex: String\n";
    sdl += "}\n\n";

    sdl += "input IntFilter {\n";
    sdl += "  eq: Int\n";
    sdl += "  ne: Int\n";
    sdl += "  lt: Int\n";
    sdl += "  lte: Int\n";
    sdl += "  gt: Int\n";
    sdl += "  gte: Int\n";
    sdl += "  in: [Int!]\n";
    sdl += "}\n\n";

    sdl += "input FloatFilter {\n";
    sdl += "  eq: Float\n";
    sdl += "  ne: Float\n";
    sdl += "  lt: Float\n";
    sdl += "  lte: Float\n";
    sdl += "  gt: Float\n";
    sdl += "  gte: Float\n";
    sdl += "}\n\n";

    sdl += "input DateFilter {\n";
    sdl += "  eq: Date\n";
    sdl += "  ne: Date\n";
    sdl += "  lt: Date\n";
    sdl += "  lte: Date\n";
    sdl += "  gt: Date\n";
    sdl += "  gte: Date\n";
    sdl += "}\n\n";

    sdl += "input TimeFilter {\n";
    sdl += "  eq: Time\n";
    sdl += "  ne: Time\n";
    sdl += "  lt: Time\n";
    sdl += "  lte: Time\n";
    sdl += "  gt: Time\n";
    sdl += "  gte: Time\n";
    sdl += "}\n\n";

    sdl += "input IDFilter {\n";
    sdl += "  eq: ID\n";
    sdl += "  ne: ID\n";
    sdl += "  in: [ID!]\n";
    sdl += "}\n";

    return sdl;
  }

  // Generate introspection response for __type(name: "...") query
  // Returns the introspection JSON for a specific type, or null if not found
  nlohmann::json typeIntrospection(const std::string& typeName) const {
    // Check for Query type
    if (typeName == "Query") {
      nlohmann::json queryTypeObj;
      queryTypeObj["kind"] = "OBJECT";
      queryTypeObj["name"] = "Query";
      queryTypeObj["description"] = "The root query type";

      nlohmann::json queryFields = nlohmann::json::array();
      for (const auto& type : types_) {
        nlohmann::json field;
        field["name"] = type.graphqlName;
        field["description"] =
            type.comment.has_value() ? nlohmann::json(type.comment.value())
                                     : nlohmann::json(nullptr);
        nlohmann::json args = nlohmann::json::array();
        args.push_back(createArgument("id", "ID", false));
        args.push_back(
            createArgument("filter", type.graphqlName + "Filter", false));
        args.push_back(createArgument("first", "Int", false));
        args.push_back(createArgument("after", "String", false));
        args.push_back(
            createArgument("orderBy", type.graphqlName + "OrderBy", false));
        field["args"] = args;
        field["type"] = createListType(type.graphqlName, true, true);
        field["isDeprecated"] = false;
        field["deprecationReason"] = nullptr;
        queryFields.push_back(field);
      }
      queryTypeObj["fields"] = queryFields;
      queryTypeObj["interfaces"] = nlohmann::json::array();
      return queryTypeObj;
    }

    // Check for scalar types
    static const std::vector<std::string> scalarTypes = {
        "String", "Int", "Float", "Boolean", "ID", "DateTime", "Date", "Time"};
    for (const auto& scalar : scalarTypes) {
      if (typeName == scalar) {
        return createScalarType(scalar);
      }
    }

    // Check user-defined types
    const SchemaType* type = findType(typeName);
    if (type) {
      return typeToIntrospectionJSON(*type);
    }

    // Type not found
    return nullptr;
  }

  // Generate introspection response for __schema query
  nlohmann::json toIntrospectionJSON() const {
    nlohmann::json schema;

    // Query type
    nlohmann::json queryType;
    queryType["name"] = "Query";

    // Types array
    nlohmann::json typesArray = nlohmann::json::array();

    // Add Query type
    nlohmann::json queryTypeObj;
    queryTypeObj["kind"] = "OBJECT";
    queryTypeObj["name"] = "Query";
    queryTypeObj["description"] = nullptr;

    nlohmann::json queryFields = nlohmann::json::array();
    for (const auto& type : types_) {
      nlohmann::json field;
      field["name"] = type.graphqlName;
      field["description"] =
          type.comment.has_value() ? nlohmann::json(type.comment.value())
                                   : nlohmann::json(nullptr);

      // Arguments
      nlohmann::json args = nlohmann::json::array();
      args.push_back(createArgument("id", "ID", false));
      args.push_back(
          createArgument("filter", type.graphqlName + "Filter", false));
      args.push_back(createArgument("first", "Int", false));
      args.push_back(createArgument("after", "String", false));
      args.push_back(
          createArgument("orderBy", type.graphqlName + "OrderBy", false));
      field["args"] = args;

      // Return type: [TypeName!]!
      field["type"] = createListType(type.graphqlName, true, true);

      field["isDeprecated"] = false;
      field["deprecationReason"] = nullptr;
      queryFields.push_back(field);
    }

    // Add introspection fields
    nlohmann::json schemaField;
    schemaField["name"] = "__schema";
    schemaField["description"] = nullptr;
    schemaField["args"] = nlohmann::json::array();
    schemaField["type"] = createNonNullType("__Schema");
    schemaField["isDeprecated"] = false;
    schemaField["deprecationReason"] = nullptr;
    queryFields.push_back(schemaField);

    nlohmann::json typeField;
    typeField["name"] = "__type";
    typeField["description"] = nullptr;
    nlohmann::json typeArgs = nlohmann::json::array();
    typeArgs.push_back(createArgument("name", "String", true));
    typeField["args"] = typeArgs;
    typeField["type"] = createNamedType("__Type");
    typeField["isDeprecated"] = false;
    typeField["deprecationReason"] = nullptr;
    queryFields.push_back(typeField);

    queryTypeObj["fields"] = queryFields;
    queryTypeObj["interfaces"] = nlohmann::json::array();
    typesArray.push_back(queryTypeObj);

    // Add user-defined types
    for (const auto& type : types_) {
      typesArray.push_back(typeToIntrospectionJSON(type));
    }

    // Add scalar types
    typesArray.push_back(createScalarType("String"));
    typesArray.push_back(createScalarType("Int"));
    typesArray.push_back(createScalarType("Float"));
    typesArray.push_back(createScalarType("Boolean"));
    typesArray.push_back(createScalarType("ID"));
    typesArray.push_back(createScalarType("DateTime"));
    typesArray.push_back(createScalarType("Date"));
    typesArray.push_back(createScalarType("Time"));

    schema["queryType"] = queryType;
    schema["mutationType"] = nullptr;
    schema["subscriptionType"] = nullptr;
    schema["types"] = typesArray;
    schema["directives"] = nlohmann::json::array();

    return schema;
  }

 private:
  std::vector<SchemaType> types_;
  // Map from GraphQL name to index in types_
  std::unordered_map<std::string, size_t> typesByName_;
  // Map from class IRI to index in types_
  std::unordered_map<std::string, size_t> typesByIRI_;

  // Mutation-related members
  std::vector<MutationField> mutationFields_;
  std::unordered_map<std::string, size_t> mutationsByName_;
  std::vector<InputType> inputTypes_;
  std::unordered_map<std::string, size_t> inputTypesByName_;
  IriConfig iriConfig_;
  bool versioningEnabled_ = false;
  bool timestampsEnabled_ = false;

  // Helper functions for JSON introspection

  static nlohmann::json createArgument(const std::string& name,
                                       const std::string& typeName,
                                       bool required) {
    nlohmann::json arg;
    arg["name"] = name;
    arg["description"] = nullptr;
    arg["type"] = required ? createNonNullType(typeName)
                           : createNamedType(typeName);
    arg["defaultValue"] = nullptr;
    return arg;
  }

  static nlohmann::json createNamedType(const std::string& name) {
    nlohmann::json type;
    type["kind"] = "SCALAR";
    type["name"] = name;
    type["ofType"] = nullptr;
    return type;
  }

  static nlohmann::json createNonNullType(const std::string& name) {
    nlohmann::json type;
    type["kind"] = "NON_NULL";
    type["name"] = nullptr;
    type["ofType"] = createNamedType(name);
    return type;
  }

  static nlohmann::json createListType(const std::string& elementType,
                                       bool elementRequired,
                                       bool listRequired) {
    nlohmann::json innerType;
    if (elementRequired) {
      innerType = createNonNullType(elementType);
    } else {
      innerType = createNamedType(elementType);
    }

    nlohmann::json listType;
    listType["kind"] = "LIST";
    listType["name"] = nullptr;
    listType["ofType"] = innerType;

    if (listRequired) {
      nlohmann::json nonNullList;
      nonNullList["kind"] = "NON_NULL";
      nonNullList["name"] = nullptr;
      nonNullList["ofType"] = listType;
      return nonNullList;
    }

    return listType;
  }

  static nlohmann::json createScalarType(const std::string& name) {
    nlohmann::json type;
    type["kind"] = "SCALAR";
    type["name"] = name;
    type["description"] = nullptr;
    return type;
  }

  nlohmann::json typeToIntrospectionJSON(const SchemaType& type) const {
    nlohmann::json typeObj;
    typeObj["kind"] = "OBJECT";
    typeObj["name"] = type.graphqlName;
    typeObj["description"] =
        type.comment.has_value() ? nlohmann::json(type.comment.value())
                                 : nlohmann::json(nullptr);

    nlohmann::json fields = nlohmann::json::array();

    // Add id field
    nlohmann::json idField;
    idField["name"] = "id";
    idField["description"] = "The unique identifier (IRI) of this resource";
    idField["args"] = nlohmann::json::array();
    idField["type"] = createNonNullType("ID");
    idField["isDeprecated"] = false;
    idField["deprecationReason"] = nullptr;
    fields.push_back(idField);

    // Add user-defined fields
    for (const auto& field : type.fields) {
      nlohmann::json fieldObj;
      fieldObj["name"] = field.graphqlName;
      fieldObj["description"] =
          field.comment.has_value() ? nlohmann::json(field.comment.value())
                                    : nlohmann::json(nullptr);

      // Arguments
      nlohmann::json args = nlohmann::json::array();
      if (field.scalarType == ScalarType::STRING && !field.isObjectProperty) {
        args.push_back(createArgument("lang", "String", false));
      }
      fieldObj["args"] = args;

      // Type
      std::string typeName;
      if (field.isObjectProperty) {
        typeName = field.objectTypeName;
      } else {
        typeName = scalarTypeToString(field.scalarType);
      }

      if (field.isList) {
        fieldObj["type"] =
            createListType(typeName, field.isRequired, field.isRequired);
      } else if (field.isRequired) {
        fieldObj["type"] = createNonNullType(typeName);
      } else {
        fieldObj["type"] = createNamedType(typeName);
      }

      fieldObj["isDeprecated"] = false;
      fieldObj["deprecationReason"] = nullptr;
      fields.push_back(fieldObj);
    }

    typeObj["fields"] = fields;

    // Interfaces
    nlohmann::json interfaces = nlohmann::json::array();
    for (const auto& superType : type.superTypes) {
      nlohmann::json iface;
      iface["kind"] = "INTERFACE";
      iface["name"] = superType;
      iface["ofType"] = nullptr;
      interfaces.push_back(iface);
    }
    typeObj["interfaces"] = interfaces;

    return typeObj;
  }
};

}  // namespace graphql

#endif  // QLEVER_SRC_PARSER_GRAPHQL_GRAPHQLSCHEMA_H
