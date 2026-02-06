// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifdef QLEVER_GRAPHQL_SUPPORT

#include "engine/graphql/GraphQLResultFormatter.h"

#include <regex>
#include <sstream>

#include "engine/ExportQueryExecutionTrees.h"
#include "index/Index.h"

namespace graphql {

// ____________________________________________________________________________
nlohmann::json GraphQLResultFormatter::format(
    const Result& result,
    const TranslationResult& translationResult,
    const Operation& operation,
    const Index& index) {
  nlohmann::json data = nlohmann::json::object();

  // Build field mappings from the operation
  auto mappings = buildFieldMappings(operation, translationResult);

  if (mappings.empty()) {
    return createResponse(data);
  }

  // Get column names from the translation result
  std::vector<std::string> columnNames;
  for (const auto& [varName, colInfo] : translationResult.variableToColumn) {
    // Ensure we have space for this column
    if (colInfo.columnIndex_ >= columnNames.size()) {
      columnNames.resize(colInfo.columnIndex_ + 1);
    }
    columnNames[colInfo.columnIndex_] = varName;
  }

  // Process each row and build entity map
  // Key: entity ID, Value: JSON object for that entity
  std::unordered_map<std::string, nlohmann::json> entityMap;

  // Get the result data
  const auto& idTable = result.idTable();
  const auto& localVocab = result.localVocab();

  // Convert each row to strings and process
  for (size_t rowIdx = 0; rowIdx < idTable.numRows(); ++rowIdx) {
    std::vector<std::string> row;
    row.reserve(idTable.numColumns());

    for (size_t colIdx = 0; colIdx < idTable.numColumns(); ++colIdx) {
      Id id = idTable(rowIdx, colIdx);
      auto stringAndType = ExportQueryExecutionTrees::idToStringAndType(
          index, id, localVocab);
      if (stringAndType.has_value()) {
        row.push_back(stringAndType->first);
      } else {
        row.push_back("");
      }
    }

    // Process this row
    processRow(row, columnNames, mappings, entityMap);
  }

  // Build the nested result structure
  for (const auto& selection : operation.selectionSet) {
    if (auto* fieldPtr = std::get_if<std::shared_ptr<Field>>(&selection)) {
      const auto& field = **fieldPtr;
      const std::string key = field.responseKey();

      // For root-level fields, create an array
      data[key] = nlohmann::json::array();

      // Add entities from the map
      for (const auto& [entityId, entityJson] : entityMap) {
        data[key].push_back(entityJson);
      }
    }
  }

  return createResponse(data);
}

// ____________________________________________________________________________
nlohmann::json GraphQLResultFormatter::formatErrors(
    const std::vector<GraphQLError>& errors) {
  nlohmann::json response;
  response["data"] = nullptr;
  response["errors"] = nlohmann::json::array();

  for (const auto& error : errors) {
    response["errors"].push_back(error.toJSON());
  }

  return response;
}

// ____________________________________________________________________________
nlohmann::json GraphQLResultFormatter::createResponse(
    const nlohmann::json& data,
    const std::vector<GraphQLError>& errors) {
  nlohmann::json response;
  response["data"] = data;

  if (!errors.empty()) {
    response["errors"] = nlohmann::json::array();
    for (const auto& error : errors) {
      response["errors"].push_back(error.toJSON());
    }
  }

  return response;
}

// ____________________________________________________________________________
std::vector<FieldMapping> GraphQLResultFormatter::buildFieldMappings(
    const Operation& operation,
    const TranslationResult& translationResult) {
  std::vector<FieldMapping> mappings;

  for (const auto& selection : operation.selectionSet) {
    if (auto* fieldPtr = std::get_if<std::shared_ptr<Field>>(&selection)) {
      const auto& field = **fieldPtr;
      // Skip introspection fields at the root level
      if (field.name == "__schema" || field.name == "__type") {
        continue;
      }
      mappings.push_back(
          buildFieldMapping(field, translationResult, "", ""));
    }
  }

  return mappings;
}

// ____________________________________________________________________________
FieldMapping GraphQLResultFormatter::buildFieldMapping(
    const Field& field,
    const TranslationResult& translationResult,
    const std::string& parentVar,
    const std::string& parentTypeName) {
  FieldMapping mapping;
  mapping.graphqlKey = field.responseKey();
  mapping.parentVar = parentVar;

  // Handle __typename introspection field
  if (field.name == "__typename") {
    mapping.sparqlVar = "__typename";  // Special marker
    mapping.isNested = false;
    return mapping;
  }

  // Set type name - for root fields, use the field name as the type
  // For nested fields, we'll get it from the translation result
  if (parentVar.empty()) {
    // Root field - the field name is the type name
    mapping.typeName = field.name;
  } else {
    // Nested field - try to get from translation result's rootMapping
    mapping.typeName = parentTypeName;
  }

  // Find the SPARQL variable for this field
  auto it = translationResult.fieldToVariable.find(field.name);
  if (it != translationResult.fieldToVariable.end()) {
    mapping.sparqlVar = it->second;
  } else {
    // Use the field name as the variable name
    mapping.sparqlVar = field.name;
  }

  // Check if this field has nested selections
  mapping.isNested = !field.selectionSet.empty();

  // Process child fields
  for (const auto& childSelection : field.selectionSet) {
    if (auto* fieldPtr = std::get_if<std::shared_ptr<Field>>(&childSelection)) {
      const auto& childField = **fieldPtr;
      mapping.children.push_back(
          buildFieldMapping(childField, translationResult, mapping.sparqlVar,
                            mapping.typeName));
    }
  }

  return mapping;
}

// ____________________________________________________________________________
void GraphQLResultFormatter::processRow(
    const std::vector<std::string>& row,
    const std::vector<std::string>& columnNames,
    const std::vector<FieldMapping>& mappings,
    std::unordered_map<std::string, nlohmann::json>& entityMap) {
  // Build a map from column name to value for this row
  std::unordered_map<std::string, std::string> rowData;
  for (size_t i = 0; i < row.size() && i < columnNames.size(); ++i) {
    rowData[columnNames[i]] = row[i];
  }

  // Process each root-level mapping
  for (const auto& mapping : mappings) {
    auto varIt = rowData.find(mapping.sparqlVar);
    if (varIt == rowData.end() || varIt->second.empty()) {
      continue;
    }

    const std::string& entityId = varIt->second;

    // Get or create the entity JSON object
    auto& entityJson = entityMap[entityId];
    if (entityJson.is_null()) {
      entityJson = nlohmann::json::object();
      entityJson["id"] = sparqlValueToJson(entityId);
    }

    // Process child fields
    for (const auto& child : mapping.children) {
      // Handle __typename introspection field
      if (child.sparqlVar == "__typename") {
        entityJson[child.graphqlKey] = mapping.typeName.empty()
                                           ? mapping.graphqlKey
                                           : mapping.typeName;
        continue;
      }

      auto childVarIt = rowData.find(child.sparqlVar);
      if (childVarIt == rowData.end() || childVarIt->second.empty()) {
        continue;
      }

      const std::string& childKey = child.graphqlKey;
      const std::string& childValue = childVarIt->second;

      if (child.isNested) {
        // Nested object - need to collect into array
        if (!entityJson.contains(childKey)) {
          entityJson[childKey] = nlohmann::json::array();
        }

        // Check if we already have this nested entity
        bool found = false;
        for (auto& existing : entityJson[childKey]) {
          if (existing.contains("id") &&
              existing["id"] == sparqlValueToJson(childValue)) {
            found = true;
            break;
          }
        }

        if (!found) {
          nlohmann::json nestedObj;
          nestedObj["id"] = sparqlValueToJson(childValue);

          // Add nested fields
          for (const auto& nestedChild : child.children) {
            auto nestedVarIt = rowData.find(nestedChild.sparqlVar);
            if (nestedVarIt != rowData.end() && !nestedVarIt->second.empty()) {
              nestedObj[nestedChild.graphqlKey] =
                  sparqlValueToJson(nestedVarIt->second);
            }
          }

          entityJson[childKey].push_back(std::move(nestedObj));
        }
      } else {
        // Scalar field
        if (child.isList) {
          // Collect into array
          if (!entityJson.contains(childKey)) {
            entityJson[childKey] = nlohmann::json::array();
          }
          auto jsonValue = sparqlValueToJson(childValue);
          // Avoid duplicates
          if (std::find(entityJson[childKey].begin(),
                        entityJson[childKey].end(),
                        jsonValue) == entityJson[childKey].end()) {
            entityJson[childKey].push_back(jsonValue);
          }
        } else {
          // Single value
          entityJson[childKey] = sparqlValueToJson(childValue);
        }
      }
    }
  }
}

// ____________________________________________________________________________
nlohmann::json GraphQLResultFormatter::buildNestedResult(
    const std::unordered_map<std::string, nlohmann::json>& entityMap,
    const std::vector<FieldMapping>& mappings) {
  nlohmann::json result = nlohmann::json::array();

  for (const auto& [entityId, entityJson] : entityMap) {
    result.push_back(entityJson);
  }

  return result;
}

// ____________________________________________________________________________
nlohmann::json GraphQLResultFormatter::sparqlValueToJson(
    const std::string& value) {
  if (value.empty()) {
    return nullptr;
  }

  // Check for typed literals
  if (isTypedLiteral(value)) {
    return parseTypedLiteral(value);
  }

  // Check for IRIs
  if (isIRI(value)) {
    // Return the IRI as a string (could extract local name if preferred)
    return value;
  }

  // Check for language-tagged strings
  // Format: "text"@lang
  {
    size_t atPos = value.rfind('@');
    if (atPos != std::string::npos && atPos > 0 &&
        value.front() == '"' && value[atPos - 1] == '"') {
      // Extract the string content between quotes
      return value.substr(1, atPos - 2);
    }
  }

  // Check for simple quoted strings
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    return value.substr(1, value.size() - 2);
  }

  // Check for booleans
  if (value == "true" || value == "false") {
    return value == "true";
  }

  // Check for numbers
  try {
    // Try integer first
    size_t pos;
    long long intVal = std::stoll(value, &pos);
    if (pos == value.size()) {
      return intVal;
    }
  } catch (...) {
  }

  try {
    // Try floating point
    size_t pos;
    double floatVal = std::stod(value, &pos);
    if (pos == value.size()) {
      return floatVal;
    }
  } catch (...) {
  }

  // Default: return as string
  return value;
}

// ____________________________________________________________________________
std::string GraphQLResultFormatter::extractLocalName(const std::string& iri) {
  // Find the last # or / to get the local name
  size_t hashPos = iri.rfind('#');
  size_t slashPos = iri.rfind('/');

  size_t pos = std::string::npos;
  if (hashPos != std::string::npos && slashPos != std::string::npos) {
    pos = std::max(hashPos, slashPos);
  } else if (hashPos != std::string::npos) {
    pos = hashPos;
  } else if (slashPos != std::string::npos) {
    pos = slashPos;
  }

  if (pos != std::string::npos && pos + 1 < iri.size()) {
    std::string localName = iri.substr(pos + 1);
    // Remove trailing > if present
    if (!localName.empty() && localName.back() == '>') {
      localName.pop_back();
    }
    return localName;
  }

  return iri;
}

// ____________________________________________________________________________
bool GraphQLResultFormatter::isIRI(const std::string& value) {
  // Check for <...> format or common IRI prefixes
  if (value.size() >= 2 && value.front() == '<' && value.back() == '>') {
    return true;
  }
  // Check for prefixed names like wd:Q123 or schema:name
  static const std::regex prefixedNameRegex(R"(^\w+:\w+$)");
  return std::regex_match(value, prefixedNameRegex);
}

// ____________________________________________________________________________
bool GraphQLResultFormatter::isTypedLiteral(const std::string& value) {
  // Format: "value"^^<datatype> or "value"^^prefix:type
  return value.find("^^") != std::string::npos;
}

// ____________________________________________________________________________
nlohmann::json GraphQLResultFormatter::parseTypedLiteral(
    const std::string& value) {
  size_t typePos = value.find("^^");
  if (typePos == std::string::npos) {
    return value;
  }

  std::string lexicalValue = value.substr(0, typePos);
  std::string datatype = value.substr(typePos + 2);

  // Remove quotes from lexical value
  if (lexicalValue.size() >= 2 && lexicalValue.front() == '"' &&
      lexicalValue.back() == '"') {
    lexicalValue = lexicalValue.substr(1, lexicalValue.size() - 2);
  }

  // Handle common XSD datatypes
  if (datatype.find("integer") != std::string::npos ||
      datatype.find("int") != std::string::npos ||
      datatype.find("long") != std::string::npos ||
      datatype.find("short") != std::string::npos ||
      datatype.find("byte") != std::string::npos) {
    try {
      return std::stoll(lexicalValue);
    } catch (...) {
      return lexicalValue;
    }
  }

  if (datatype.find("decimal") != std::string::npos ||
      datatype.find("float") != std::string::npos ||
      datatype.find("double") != std::string::npos) {
    try {
      return std::stod(lexicalValue);
    } catch (...) {
      return lexicalValue;
    }
  }

  if (datatype.find("boolean") != std::string::npos) {
    return lexicalValue == "true" || lexicalValue == "1";
  }

  if (datatype.find("date") != std::string::npos ||
      datatype.find("dateTime") != std::string::npos) {
    // Return dates as strings (ISO format)
    return lexicalValue;
  }

  // Default: return as string
  return lexicalValue;
}

}  // namespace graphql

#endif  // QLEVER_GRAPHQL_SUPPORT
