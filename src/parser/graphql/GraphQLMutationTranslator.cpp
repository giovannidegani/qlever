// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#include "parser/graphql/GraphQLMutationTranslator.h"

#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace graphql {

// ============================================================================
// MutationTriple Implementation
// ============================================================================

std::string MutationTriple::toNTriples() const {
  std::string result;

  // Subject (always IRI)
  result += "<" + subject + "> ";

  // Predicate (always IRI)
  result += "<" + predicate + "> ";

  // Object (IRI or literal)
  if (objectIsLiteral) {
    result += "\"" + GraphQLMutationTranslator::escapeLiteral(object) + "\"";
    if (!langTag.empty()) {
      result += "@" + langTag;
    } else if (!datatypeIri.empty()) {
      result += "^^<" + datatypeIri + ">";
    }
  } else {
    result += "<" + object + ">";
  }

  result += " .";
  return result;
}

// ============================================================================
// InsertDataStatement Implementation
// ============================================================================

std::string InsertDataStatement::toSparql() const {
  std::ostringstream ss;
  ss << "INSERT DATA {\n";

  if (graph.has_value()) {
    ss << "  GRAPH <" << graph.value() << "> {\n";
    for (const auto& triple : triples) {
      ss << "    " << triple.toNTriples() << "\n";
    }
    ss << "  }\n";
  } else {
    for (const auto& triple : triples) {
      ss << "  " << triple.toNTriples() << "\n";
    }
  }

  ss << "}";
  return ss.str();
}

// ============================================================================
// DeleteWhereStatement Implementation
// ============================================================================

std::string DeleteWhereStatement::toSparql() const {
  std::ostringstream ss;
  ss << "DELETE WHERE {\n";

  if (graph.has_value()) {
    ss << "  GRAPH <" << graph.value() << "> {\n";
    for (const auto& pattern : patterns) {
      ss << "    " << pattern.toNTriples() << "\n";
    }
    ss << "  }\n";
  } else {
    for (const auto& pattern : patterns) {
      ss << "  " << pattern.toNTriples() << "\n";
    }
  }

  ss << "}";
  return ss.str();
}

// ============================================================================
// DeleteInsertStatement Implementation
// ============================================================================

std::string DeleteInsertStatement::toSparql() const {
  std::ostringstream ss;

  // DELETE clause
  if (!deletePatterns.empty()) {
    ss << "DELETE {\n";
    if (graph.has_value()) {
      ss << "  GRAPH <" << graph.value() << "> {\n";
      for (const auto& pattern : deletePatterns) {
        ss << "    " << pattern.toNTriples() << "\n";
      }
      ss << "  }\n";
    } else {
      for (const auto& pattern : deletePatterns) {
        ss << "  " << pattern.toNTriples() << "\n";
      }
    }
    ss << "}\n";
  }

  // INSERT clause
  if (!insertPatterns.empty()) {
    ss << "INSERT {\n";
    if (graph.has_value()) {
      ss << "  GRAPH <" << graph.value() << "> {\n";
      for (const auto& pattern : insertPatterns) {
        ss << "    " << pattern.toNTriples() << "\n";
      }
      ss << "  }\n";
    } else {
      for (const auto& pattern : insertPatterns) {
        ss << "  " << pattern.toNTriples() << "\n";
      }
    }
    ss << "}\n";
  }

  // WHERE clause
  ss << "WHERE {\n";
  if (graph.has_value()) {
    ss << "  GRAPH <" << graph.value() << "> {\n";
    for (const auto& pattern : wherePatterns) {
      ss << "    " << pattern.toNTriples() << "\n";
    }
    for (const auto& filter : filterExpressions) {
      ss << "    FILTER(" << filter << ")\n";
    }
    ss << "  }\n";
  } else {
    for (const auto& pattern : wherePatterns) {
      ss << "  " << pattern.toNTriples() << "\n";
    }
    for (const auto& filter : filterExpressions) {
      ss << "  FILTER(" << filter << ")\n";
    }
  }
  ss << "}";

  return ss.str();
}

// ============================================================================
// GraphQLMutationTranslator Implementation
// ============================================================================

GraphQLMutationTranslator::GraphQLMutationTranslator(
    const GraphQLSchema& schema)
    : schema_(schema) {}

std::variant<MutationTranslationResult, std::vector<GraphQLError>>
GraphQLMutationTranslator::translate(
    const Document& document, std::optional<std::string_view> operationName,
    const std::unordered_map<std::string, Value>& variables) {
  std::vector<GraphQLError> errors;

  // Find the operation to execute
  const Operation* targetOp = nullptr;
  size_t mutationCount = 0;

  for (const auto& op : document.operations) {
    if (op.type == OperationType::Mutation) {
      mutationCount++;
      if (operationName.has_value()) {
        if (op.name == operationName.value()) {
          targetOp = &op;
          break;
        }
      } else {
        targetOp = &op;
      }
    }
  }

  if (!targetOp) {
    if (operationName.has_value()) {
      GraphQLError err;
      err.message = "Mutation operation '" + std::string(operationName.value()) +
                    "' not found";
      err.locations.push_back(SourceLocation{1, 1});
      errors.push_back(std::move(err));
    } else if (mutationCount == 0) {
      GraphQLError err;
      err.message = "No mutation operation found in document";
      err.locations.push_back(SourceLocation{1, 1});
      errors.push_back(std::move(err));
    } else {
      GraphQLError err;
      err.message = "Multiple mutations found but no operationName provided";
      err.locations.push_back(SourceLocation{1, 1});
      errors.push_back(std::move(err));
    }
    return errors;
  }

  // Translate each field in the mutation
  // For now, we only support single mutations (first field)
  if (targetOp->selectionSet.empty()) {
    GraphQLError err;
    err.message = "Mutation has no fields";
    err.locations.push_back(SourceLocation{1, 1});
    errors.push_back(std::move(err));
    return errors;
  }

  // Get the first field (mutation call)
  const auto& selection = targetOp->selectionSet[0];
  if (!std::holds_alternative<std::shared_ptr<Field>>(selection)) {
    GraphQLError err;
    err.message = "Expected mutation field";
    err.locations.push_back(SourceLocation{1, 1});
    errors.push_back(std::move(err));
    return errors;
  }

  const auto& fieldPtr = std::get<std::shared_ptr<Field>>(selection);
  return translateMutationField(*fieldPtr, variables);
}

std::variant<MutationTranslationResult, std::vector<GraphQLError>>
GraphQLMutationTranslator::translateMutationField(
    const Field& field,
    const std::unordered_map<std::string, Value>& variables) {
  std::vector<GraphQLError> errors;

  // Find the mutation definition in schema
  const MutationField* mutation = schema_.findMutation(field.name);
  if (!mutation) {
    GraphQLError err;
    err.message = "Unknown mutation: " + field.name;
    err.locations.push_back(field.location);
    errors.push_back(std::move(err));
    return errors;
  }

  // Dispatch to appropriate handler based on mutation type
  switch (mutation->mutationType) {
    case MutationType::Create:
      return translateCreate(*mutation, field, variables);
    case MutationType::Update:
      return translateUpdate(*mutation, field, variables);
    case MutationType::Delete:
      return translateDelete(*mutation, field, variables);
    case MutationType::Upsert:
      return translateUpsert(*mutation, field, variables);
    case MutationType::BatchCreate:
      return translateBatchCreate(*mutation, field, variables);
    case MutationType::BatchDelete:
      return translateBatchDelete(*mutation, field, variables);
  }

  GraphQLError err;
  err.message = "Unknown mutation type";
  err.locations.push_back(field.location);
  errors.push_back(std::move(err));
  return errors;
}

// ============================================================================
// Mutation Type Translators
// ============================================================================

MutationTranslationResult GraphQLMutationTranslator::translateCreate(
    const MutationField& mutation, const Field& field,
    const std::unordered_map<std::string, Value>& variables) {
  MutationTranslationResult result;
  result.mutationType = MutationType::Create;
  result.entityTypeName = mutation.targetTypeName;
  result.returnFields = collectReturnFields(field);

  // Get input data
  auto inputData = extractInputData(field.arguments, "input", variables);
  if (!inputData.has_value()) {
    inputData = nlohmann::json::object();
  }

  // Generate IRI for new entity
  std::string entityIri = generateIri(mutation.targetTypeName, inputData.value());
  result.entityIri = entityIri;

  // Get type schema
  const SchemaType* type = schema_.findType(mutation.targetTypeName);
  if (!type) {
    return result;  // Error handling should be done elsewhere
  }

  // Get current timestamp
  std::string timestamp = getCurrentTimestamp();

  // Build INSERT DATA statement
  InsertDataStatement insertStmt;
  insertStmt.triples = buildCreateTriples(entityIri, *type, inputData.value(),
                                          timestamp);

  result.statements.push_back(std::move(insertStmt));
  return result;
}

MutationTranslationResult GraphQLMutationTranslator::translateUpdate(
    const MutationField& mutation, const Field& field,
    const std::unordered_map<std::string, Value>& variables) {
  MutationTranslationResult result;
  result.mutationType = MutationType::Update;
  result.entityTypeName = mutation.targetTypeName;
  result.returnFields = collectReturnFields(field);

  // Get entity ID
  auto id = extractId(field.arguments, variables);
  if (!id.has_value()) {
    return result;
  }

  result.entityIri = id.value();

  // Get input data
  auto inputData = extractInputData(field.arguments, "input", variables);
  if (!inputData.has_value()) {
    return result;
  }

  // Get type schema
  const SchemaType* type = schema_.findType(mutation.targetTypeName);
  if (!type) {
    return result;
  }

  // Get current timestamp
  std::string timestamp = getCurrentTimestamp();

  // Build DELETE/INSERT WHERE statement
  auto [deletePatterns, insertPatterns] =
      buildUpdateTriples(result.entityIri, *type, inputData.value(), timestamp);

  if (!deletePatterns.empty() || !insertPatterns.empty()) {
    DeleteInsertStatement updateStmt;
    updateStmt.deletePatterns = std::move(deletePatterns);
    updateStmt.insertPatterns = std::move(insertPatterns);

    // WHERE clause ensures entity exists
    updateStmt.wherePatterns.push_back(MutationTriple{
        result.entityIri,
        "http://www.w3.org/1999/02/22-rdf-syntax-ns#type",
        type->classIRI});

    // Version check if versioning is enabled
    if (schema_.isVersioningEnabled()) {
      auto versionValue = inputData.value().find("_version");
      if (versionValue != inputData.value().end() && versionValue->is_number()) {
        auto versionPatterns =
            buildVersionCheckPattern(result.entityIri, versionValue->get<int64_t>());
        for (auto& pattern : versionPatterns) {
          updateStmt.wherePatterns.push_back(std::move(pattern));
        }
      }
    }

    result.statements.push_back(std::move(updateStmt));
  }

  return result;
}

MutationTranslationResult GraphQLMutationTranslator::translateDelete(
    const MutationField& mutation, const Field& field,
    const std::unordered_map<std::string, Value>& variables) {
  MutationTranslationResult result;
  result.mutationType = MutationType::Delete;
  result.entityTypeName = mutation.targetTypeName;
  result.returnFields = collectReturnFields(field);

  // Get entity ID
  auto id = extractId(field.arguments, variables);
  if (!id.has_value()) {
    return result;
  }

  result.entityIri = id.value();

  // Get type schema
  const SchemaType* type = schema_.findType(mutation.targetTypeName);
  if (!type) {
    return result;
  }

  // Build DELETE WHERE statement that removes all triples with entity as subject
  DeleteWhereStatement deleteStmt;
  deleteStmt.patterns = buildDeleteAllTriples(result.entityIri, *type);

  result.statements.push_back(std::move(deleteStmt));
  return result;
}

MutationTranslationResult GraphQLMutationTranslator::translateUpsert(
    const MutationField& mutation, const Field& field,
    const std::unordered_map<std::string, Value>& variables) {
  MutationTranslationResult result;
  result.mutationType = MutationType::Upsert;
  result.entityTypeName = mutation.targetTypeName;
  result.returnFields = collectReturnFields(field);

  // For upsert, we need: where (match condition), create (data for new), update (data for existing)
  auto whereData = extractInputData(field.arguments, "where", variables);
  auto createData = extractInputData(field.arguments, "create", variables);
  auto updateData = extractInputData(field.arguments, "update", variables);

  // Get type schema
  const SchemaType* type = schema_.findType(mutation.targetTypeName);
  if (!type) {
    return result;
  }

  std::string timestamp = getCurrentTimestamp();

  // Check if entity exists using where condition
  // If entity has an ID in where, use that
  std::string entityIri;
  if (whereData.has_value() && whereData.value().contains("id")) {
    entityIri = whereData.value()["id"].get<std::string>();
  } else {
    // Generate new IRI if creating
    entityIri = generateIri(mutation.targetTypeName,
                            createData.value_or(nlohmann::json::object()));
  }

  result.entityIri = entityIri;

  // For SPARQL, upsert is typically:
  // DELETE { old values } INSERT { new values } WHERE { entity exists }
  // Combined with INSERT DATA if entity doesn't exist

  // First statement: UPDATE existing (DELETE/INSERT WHERE)
  if (updateData.has_value()) {
    auto [deletePatterns, insertPatterns] =
        buildUpdateTriples(entityIri, *type, updateData.value(), timestamp);

    if (!deletePatterns.empty() || !insertPatterns.empty()) {
      DeleteInsertStatement updateStmt;
      updateStmt.deletePatterns = std::move(deletePatterns);
      updateStmt.insertPatterns = std::move(insertPatterns);
      updateStmt.wherePatterns.push_back(MutationTriple{
          entityIri,
          "http://www.w3.org/1999/02/22-rdf-syntax-ns#type",
          type->classIRI});
      result.statements.push_back(std::move(updateStmt));
    }
  }

  // Second statement: CREATE if not exists
  // This uses INSERT WHERE NOT EXISTS pattern
  if (createData.has_value()) {
    InsertDataStatement insertStmt;
    insertStmt.triples =
        buildCreateTriples(entityIri, *type, createData.value(), timestamp);
    result.statements.push_back(std::move(insertStmt));
  }

  return result;
}

MutationTranslationResult GraphQLMutationTranslator::translateBatchCreate(
    const MutationField& mutation, const Field& field,
    const std::unordered_map<std::string, Value>& variables) {
  MutationTranslationResult result;
  result.mutationType = MutationType::BatchCreate;
  result.entityTypeName = mutation.targetTypeName;
  result.returnFields = collectReturnFields(field);

  // Get input array
  auto inputArray = extractInputData(field.arguments, "input", variables);
  if (!inputArray.has_value() || !inputArray.value().is_array()) {
    return result;
  }

  // Get type schema
  const SchemaType* type = schema_.findType(mutation.targetTypeName);
  if (!type) {
    return result;
  }

  std::string timestamp = getCurrentTimestamp();

  // Create a single INSERT DATA statement with all triples
  InsertDataStatement insertStmt;

  for (const auto& item : inputArray.value()) {
    std::string entityIri = generateIri(mutation.targetTypeName, item);
    result.batchEntityIris.push_back(entityIri);

    auto triples = buildCreateTriples(entityIri, *type, item, timestamp);
    for (auto& triple : triples) {
      insertStmt.triples.push_back(std::move(triple));
    }
  }

  if (!insertStmt.triples.empty()) {
    result.statements.push_back(std::move(insertStmt));
  }

  return result;
}

MutationTranslationResult GraphQLMutationTranslator::translateBatchDelete(
    const MutationField& mutation, const Field& field,
    const std::unordered_map<std::string, Value>& variables) {
  MutationTranslationResult result;
  result.mutationType = MutationType::BatchDelete;
  result.entityTypeName = mutation.targetTypeName;
  result.returnFields = collectReturnFields(field);

  // Get IDs array
  auto idsArg = extractInputData(field.arguments, "ids", variables);
  if (!idsArg.has_value() || !idsArg.value().is_array()) {
    return result;
  }

  // Get type schema
  const SchemaType* type = schema_.findType(mutation.targetTypeName);
  if (!type) {
    return result;
  }

  // Create DELETE WHERE statements for each ID
  for (const auto& idValue : idsArg.value()) {
    if (idValue.is_string()) {
      std::string entityIri = idValue.get<std::string>();
      result.batchEntityIris.push_back(entityIri);

      DeleteWhereStatement deleteStmt;
      deleteStmt.patterns = buildDeleteAllTriples(entityIri, *type);
      result.statements.push_back(std::move(deleteStmt));
    }
  }

  return result;
}

// ============================================================================
// Helper Methods
// ============================================================================

std::string GraphQLMutationTranslator::generateIri(
    const std::string& typeName, const nlohmann::json& inputData) {
  const auto& config = schema_.getIriConfig();

  switch (config.strategy) {
    case IriStrategy::UserProvided: {
      // Look for 'id' in input data
      if (inputData.contains("id") && inputData["id"].is_string()) {
        return inputData["id"].get<std::string>();
      }
      // Fall through to UUID if no ID provided
      [[fallthrough]];
    }

    case IriStrategy::UUID: {
      // Generate UUID v4
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<> dis(0, 15);
      std::uniform_int_distribution<> dis2(8, 11);

      std::stringstream ss;
      ss << std::hex;

      for (int i = 0; i < 8; i++) ss << dis(gen);
      ss << "-";
      for (int i = 0; i < 4; i++) ss << dis(gen);
      ss << "-4";
      for (int i = 0; i < 3; i++) ss << dis(gen);
      ss << "-";
      ss << dis2(gen);
      for (int i = 0; i < 3; i++) ss << dis(gen);
      ss << "-";
      for (int i = 0; i < 12; i++) ss << dis(gen);

      std::string uuid = ss.str();
      std::string baseIri = config.baseIri.empty()
                                ? "http://example.org/"
                                : config.baseIri;

      return baseIri + typeName + "/" + uuid;
    }

    case IriStrategy::Template: {
      // Use template pattern like "{baseIri}/{typeName}/{fieldValue}"
      std::string result = config.templatePattern;

      // Replace {typeName}
      size_t pos = result.find("{typeName}");
      if (pos != std::string::npos) {
        result.replace(pos, 10, typeName);
      }

      // Replace {baseIri}
      pos = result.find("{baseIri}");
      if (pos != std::string::npos) {
        std::string baseIri = config.baseIri.empty()
                                  ? "http://example.org/"
                                  : config.baseIri;
        result.replace(pos, 9, baseIri);
      }

      // Replace field references like {email}, {name}, etc.
      for (auto& [key, value] : inputData.items()) {
        std::string placeholder = "{" + key + "}";
        pos = result.find(placeholder);
        if (pos != std::string::npos && value.is_string()) {
          result.replace(pos, placeholder.length(), value.get<std::string>());
        }
      }

      // If any unreplaced placeholders, add unique suffix
      if (result.find('{') != std::string::npos) {
        result = config.baseIri + typeName + "/" + std::to_string(iriCounter_++);
      }

      return result;
    }
  }

  // Default fallback
  return config.baseIri + typeName + "/" + std::to_string(iriCounter_++);
}

std::string GraphQLMutationTranslator::getCurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto time_t = std::chrono::system_clock::to_time_t(now);
  std::tm tm = *std::gmtime(&time_t);

  std::ostringstream ss;
  ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

nlohmann::json GraphQLMutationTranslator::parseInputData(
    const Argument& arg,
    const std::unordered_map<std::string, Value>& variables) {
  return valueToJson(arg.value, variables);
}

nlohmann::json GraphQLMutationTranslator::valueToJson(
    const Value& value,
    const std::unordered_map<std::string, Value>& variables) {
  switch (value.type) {
    case Value::Type::Null:
      return nullptr;

    case Value::Type::Int:
      return value.asInt();

    case Value::Type::Float:
      return value.asFloat();

    case Value::Type::String:
    case Value::Type::Enum:
      return value.asString();

    case Value::Type::Boolean:
      return value.asBool();

    case Value::Type::List: {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& item : value.asList()) {
        arr.push_back(valueToJson(item, variables));
      }
      return arr;
    }

    case Value::Type::Object: {
      nlohmann::json obj = nlohmann::json::object();
      for (const auto& [key, val] : value.asObject()) {
        obj[key] = valueToJson(val, variables);
      }
      return obj;
    }
  }

  return nullptr;
}

std::vector<MutationTriple> GraphQLMutationTranslator::buildCreateTriples(
    const std::string& subjectIri, const SchemaType& type,
    const nlohmann::json& inputData, const std::string& timestamp) {
  std::vector<MutationTriple> triples;

  // Add rdf:type triple
  triples.push_back(MutationTriple{
      subjectIri,
      "http://www.w3.org/1999/02/22-rdf-syntax-ns#type",
      type.classIRI});

  // Add data triples for each field in input
  for (const auto& field : type.fields) {
    auto it = inputData.find(field.graphqlName);
    if (it != inputData.end() && !it->is_null()) {
      if (field.isObjectProperty) {
        // Relation field - value should be IRI(s)
        if (it->is_array()) {
          for (const auto& item : *it) {
            if (item.is_string()) {
              triples.push_back(MutationTriple{
                  subjectIri, field.propertyIRI, item.get<std::string>()});
            }
          }
        } else if (it->is_string()) {
          triples.push_back(MutationTriple{
              subjectIri, field.propertyIRI, it->get<std::string>()});
        }
      } else {
        // Scalar field
        if (field.isList && it->is_array()) {
          for (const auto& item : *it) {
            triples.push_back(
                buildLiteralTriple(subjectIri, field.propertyIRI, item,
                                   field.scalarType));
          }
        } else {
          triples.push_back(
              buildLiteralTriple(subjectIri, field.propertyIRI, *it,
                                 field.scalarType));
        }
      }
    }
  }

  // Add timestamp triples if enabled
  if (schema_.isTimestampsEnabled()) {
    // _createdAt
    triples.push_back(MutationTriple{
        subjectIri,
        "http://purl.org/dc/terms/created",
        timestamp,
        true,
        "http://www.w3.org/2001/XMLSchema#dateTime"});

    // _updatedAt
    triples.push_back(MutationTriple{
        subjectIri,
        "http://purl.org/dc/terms/modified",
        timestamp,
        true,
        "http://www.w3.org/2001/XMLSchema#dateTime"});
  }

  // Add version triple if enabled
  if (schema_.isVersioningEnabled()) {
    triples.push_back(MutationTriple{
        subjectIri,
        "http://example.org/version",
        "1",
        true,
        "http://www.w3.org/2001/XMLSchema#integer"});
  }

  return triples;
}

std::pair<std::vector<MutationTriple>, std::vector<MutationTriple>>
GraphQLMutationTranslator::buildUpdateTriples(
    const std::string& subjectIri, const SchemaType& type,
    const nlohmann::json& inputData, const std::string& timestamp) {
  std::vector<MutationTriple> deletePatterns;
  std::vector<MutationTriple> insertPatterns;

  // For each field in input, delete old value and insert new
  for (const auto& field : type.fields) {
    auto it = inputData.find(field.graphqlName);
    if (it != inputData.end()) {
      // Delete old value using a variable
      std::string varName = "?old_" + field.graphqlName;
      deletePatterns.push_back(MutationTriple{
          subjectIri, field.propertyIRI, varName});

      // Insert new value (unless null)
      if (!it->is_null()) {
        if (field.isObjectProperty) {
          if (it->is_array()) {
            for (const auto& item : *it) {
              if (item.is_string()) {
                insertPatterns.push_back(MutationTriple{
                    subjectIri, field.propertyIRI, item.get<std::string>()});
              }
            }
          } else if (it->is_string()) {
            insertPatterns.push_back(MutationTriple{
                subjectIri, field.propertyIRI, it->get<std::string>()});
          }
        } else {
          if (field.isList && it->is_array()) {
            for (const auto& item : *it) {
              insertPatterns.push_back(
                  buildLiteralTriple(subjectIri, field.propertyIRI, item,
                                     field.scalarType));
            }
          } else {
            insertPatterns.push_back(
                buildLiteralTriple(subjectIri, field.propertyIRI, *it,
                                   field.scalarType));
          }
        }
      }
    }
  }

  // Update timestamp if enabled
  if (schema_.isTimestampsEnabled()) {
    deletePatterns.push_back(MutationTriple{
        subjectIri,
        "http://purl.org/dc/terms/modified",
        "?old_modified"});

    insertPatterns.push_back(MutationTriple{
        subjectIri,
        "http://purl.org/dc/terms/modified",
        timestamp,
        true,
        "http://www.w3.org/2001/XMLSchema#dateTime"});
  }

  // Increment version if enabled
  if (schema_.isVersioningEnabled()) {
    deletePatterns.push_back(MutationTriple{
        subjectIri,
        "http://example.org/version",
        "?old_version"});

    // Note: In practice, you'd need a BIND expression to increment
    // For simplicity, we'd need the current version from the input
    auto versionIt = inputData.find("_version");
    int64_t newVersion = 1;
    if (versionIt != inputData.end() && versionIt->is_number()) {
      newVersion = versionIt->get<int64_t>() + 1;
    }

    insertPatterns.push_back(MutationTriple{
        subjectIri,
        "http://example.org/version",
        std::to_string(newVersion),
        true,
        "http://www.w3.org/2001/XMLSchema#integer"});
  }

  return {deletePatterns, insertPatterns};
}

std::vector<MutationTriple> GraphQLMutationTranslator::buildDeleteAllTriples(
    const std::string& subjectIri, const SchemaType& type) {
  std::vector<MutationTriple> patterns;

  // Delete type triple
  patterns.push_back(MutationTriple{
      subjectIri,
      "http://www.w3.org/1999/02/22-rdf-syntax-ns#type",
      type.classIRI});

  // Delete all property triples
  for (const auto& field : type.fields) {
    patterns.push_back(MutationTriple{
        subjectIri, field.propertyIRI, "?obj_" + field.graphqlName});
  }

  // Delete metadata triples if enabled
  if (schema_.isTimestampsEnabled()) {
    patterns.push_back(MutationTriple{
        subjectIri,
        "http://purl.org/dc/terms/created",
        "?created"});
    patterns.push_back(MutationTriple{
        subjectIri,
        "http://purl.org/dc/terms/modified",
        "?modified"});
  }

  if (schema_.isVersioningEnabled()) {
    patterns.push_back(MutationTriple{
        subjectIri,
        "http://example.org/version",
        "?version"});
  }

  return patterns;
}

MutationTriple GraphQLMutationTranslator::buildLiteralTriple(
    const std::string& subject, const std::string& predicate,
    const nlohmann::json& value, ScalarType scalarType) {
  MutationTriple triple;
  triple.subject = subject;
  triple.predicate = predicate;
  triple.objectIsLiteral = true;
  triple.datatypeIri = scalarTypeToXsdIri(scalarType);

  if (value.is_string()) {
    triple.object = value.get<std::string>();
  } else if (value.is_number_integer()) {
    triple.object = std::to_string(value.get<int64_t>());
  } else if (value.is_number_float()) {
    triple.object = std::to_string(value.get<double>());
  } else if (value.is_boolean()) {
    triple.object = value.get<bool>() ? "true" : "false";
  } else {
    triple.object = value.dump();
  }

  return triple;
}

std::string GraphQLMutationTranslator::scalarTypeToXsdIri(ScalarType type) {
  switch (type) {
    case ScalarType::STRING:
      return "http://www.w3.org/2001/XMLSchema#string";
    case ScalarType::INT:
      return "http://www.w3.org/2001/XMLSchema#integer";
    case ScalarType::FLOAT:
      return "http://www.w3.org/2001/XMLSchema#double";
    case ScalarType::BOOLEAN:
      return "http://www.w3.org/2001/XMLSchema#boolean";
    case ScalarType::ID:
      return "";  // IDs are typically IRIs, not typed literals
    case ScalarType::DATETIME:
      return "http://www.w3.org/2001/XMLSchema#dateTime";
    case ScalarType::DATE:
      return "http://www.w3.org/2001/XMLSchema#date";
    case ScalarType::TIME:
      return "http://www.w3.org/2001/XMLSchema#time";
  }
  return "http://www.w3.org/2001/XMLSchema#string";
}

std::vector<SparqlUpdateStatement>
GraphQLMutationTranslator::handleRelationInput(
    const std::string& subjectIri, const SchemaField& field,
    const nlohmann::json& relationInput, const std::string& timestamp) {
  std::vector<SparqlUpdateStatement> statements;

  // Handle "connect" - add links to existing entities
  if (relationInput.contains("connect")) {
    InsertDataStatement insertStmt;
    for (const auto& id : relationInput["connect"]) {
      if (id.is_string()) {
        insertStmt.triples.push_back(MutationTriple{
            subjectIri, field.propertyIRI, id.get<std::string>()});
      }
    }
    if (!insertStmt.triples.empty()) {
      statements.push_back(std::move(insertStmt));
    }
  }

  // Handle "disconnect" - remove links to existing entities
  if (relationInput.contains("disconnect")) {
    for (const auto& id : relationInput["disconnect"]) {
      if (id.is_string()) {
        DeleteWhereStatement deleteStmt;
        deleteStmt.patterns.push_back(MutationTriple{
            subjectIri, field.propertyIRI, id.get<std::string>()});
        statements.push_back(std::move(deleteStmt));
      }
    }
  }

  // Handle "set" - replace all links
  if (relationInput.contains("set")) {
    // First delete all existing links
    DeleteWhereStatement deleteStmt;
    deleteStmt.patterns.push_back(MutationTriple{
        subjectIri, field.propertyIRI, "?old_" + field.graphqlName});
    statements.push_back(std::move(deleteStmt));

    // Then add new links
    InsertDataStatement insertStmt;
    for (const auto& id : relationInput["set"]) {
      if (id.is_string()) {
        insertStmt.triples.push_back(MutationTriple{
            subjectIri, field.propertyIRI, id.get<std::string>()});
      }
    }
    if (!insertStmt.triples.empty()) {
      statements.push_back(std::move(insertStmt));
    }
  }

  // Handle "create" - create new entities and link them
  if (relationInput.contains("create")) {
    const SchemaType* relatedType = schema_.findType(field.objectTypeName);
    if (relatedType) {
      InsertDataStatement insertStmt;

      for (const auto& createInput : relationInput["create"]) {
        std::string newEntityIri = generateIri(field.objectTypeName, createInput);

        // Add link to new entity
        insertStmt.triples.push_back(MutationTriple{
            subjectIri, field.propertyIRI, newEntityIri});

        // Add triples for the new entity
        auto entityTriples =
            buildCreateTriples(newEntityIri, *relatedType, createInput, timestamp);
        for (auto& triple : entityTriples) {
          insertStmt.triples.push_back(std::move(triple));
        }
      }

      if (!insertStmt.triples.empty()) {
        statements.push_back(std::move(insertStmt));
      }
    }
  }

  // Handle "delete" - delete linked entities entirely
  if (relationInput.contains("delete")) {
    const SchemaType* relatedType = schema_.findType(field.objectTypeName);
    if (relatedType) {
      for (const auto& id : relationInput["delete"]) {
        if (id.is_string()) {
          std::string entityIri = id.get<std::string>();

          // Delete the link
          DeleteWhereStatement deleteLinkStmt;
          deleteLinkStmt.patterns.push_back(MutationTriple{
              subjectIri, field.propertyIRI, entityIri});
          statements.push_back(std::move(deleteLinkStmt));

          // Delete the entity itself
          DeleteWhereStatement deleteEntityStmt;
          deleteEntityStmt.patterns = buildDeleteAllTriples(entityIri, *relatedType);
          statements.push_back(std::move(deleteEntityStmt));
        }
      }
    }
  }

  return statements;
}

std::optional<std::string> GraphQLMutationTranslator::extractId(
    const std::vector<Argument>& arguments,
    const std::unordered_map<std::string, Value>& variables) {
  for (const auto& arg : arguments) {
    if (arg.name == "id") {
      auto json = valueToJson(arg.value, variables);
      if (json.is_string()) {
        return json.get<std::string>();
      }
    }
  }
  return std::nullopt;
}

std::optional<nlohmann::json> GraphQLMutationTranslator::extractInputData(
    const std::vector<Argument>& arguments, const std::string& argName,
    const std::unordered_map<std::string, Value>& variables) {
  for (const auto& arg : arguments) {
    if (arg.name == argName) {
      return valueToJson(arg.value, variables);
    }
  }
  return std::nullopt;
}

std::vector<std::string> GraphQLMutationTranslator::collectReturnFields(
    const Field& field) {
  std::vector<std::string> fields;
  for (const auto& selection : field.selectionSet) {
    if (std::holds_alternative<std::shared_ptr<Field>>(selection)) {
      const auto& subFieldPtr = std::get<std::shared_ptr<Field>>(selection);
      fields.push_back(subFieldPtr->name);
    }
  }
  return fields;
}

std::vector<MutationTriple> GraphQLMutationTranslator::buildVersionCheckPattern(
    const std::string& subjectIri, int64_t expectedVersion) {
  std::vector<MutationTriple> patterns;
  patterns.push_back(MutationTriple{
      subjectIri,
      "http://example.org/version",
      std::to_string(expectedVersion),
      true,
      "http://www.w3.org/2001/XMLSchema#integer"});
  return patterns;
}

MutationTriple GraphQLMutationTranslator::buildVersionTriple(
    const std::string& subjectIri, int64_t newVersion) {
  return MutationTriple{
      subjectIri,
      "http://example.org/version",
      std::to_string(newVersion),
      true,
      "http://www.w3.org/2001/XMLSchema#integer"};
}

std::string GraphQLMutationTranslator::escapeIri(const std::string& iri) {
  // IRIs in SPARQL are enclosed in < > and certain characters must be escaped
  std::string result;
  result.reserve(iri.size());
  for (char c : iri) {
    switch (c) {
      case '<':
        result += "%3C";
        break;
      case '>':
        result += "%3E";
        break;
      case '"':
        result += "%22";
        break;
      case ' ':
        result += "%20";
        break;
      case '{':
        result += "%7B";
        break;
      case '}':
        result += "%7D";
        break;
      case '|':
        result += "%7C";
        break;
      case '^':
        result += "%5E";
        break;
      case '`':
        result += "%60";
        break;
      case '\\':
        result += "%5C";
        break;
      default:
        result += c;
    }
  }
  return result;
}

std::string GraphQLMutationTranslator::escapeLiteral(const std::string& str) {
  std::string result;
  result.reserve(str.size() * 1.1);  // Assume ~10% escaping overhead
  for (char c : str) {
    switch (c) {
      case '\\':
        result += "\\\\";
        break;
      case '"':
        result += "\\\"";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        result += c;
    }
  }
  return result;
}

}  // namespace graphql
