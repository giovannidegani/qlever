// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifdef QLEVER_GRAPHQL_SUPPORT

#include "engine/graphql/SchemaBuilder.h"

#include <algorithm>
#include <filesystem>
#include <regex>
#include <sstream>

#include "engine/ExportQueryExecutionTrees.h"
#include "engine/MaterializedViews.h"
#include "engine/QueryExecutionContext.h"
#include "engine/QueryPlanner.h"
#include "engine/SortPerformanceEstimator.h"
#include "parser/SparqlParser.h"
#include "util/CancellationHandle.h"
#include "util/Log.h"

namespace graphql {

// ____________________________________________________________________________
SchemaBuilder::SchemaBuilder(const Index& index, SchemaBuilderConfig config)
    : index_(index), config_(std::move(config)) {}

// ____________________________________________________________________________
GraphQLSchema SchemaBuilder::build() {
  GraphQLSchema schema;

  // Initialize OWL views if configured
  if (config_.useOwlInference) {
    initializeOwlViews(false);
  }

  // Discover types - use OWL inference if available
  std::vector<DiscoveredType> types;
  if (config_.useOwlInference && hasOwlViews()) {
    AD_LOG_INFO << "Building schema with OWL inference views..." << std::endl;
    types = discoverTypesWithOwl();
  } else {
    types = discoverTypes();
  }

  // Build schema types from discovered RDF classes
  buildSchemaTypes(types, schema);

  // Build GraphQL interfaces from class hierarchy if enabled
  if (config_.generateInterfaces && hasOwlViews()) {
    buildInterfacesFromOwl(schema);
  }

  return schema;
}

// ____________________________________________________________________________
const GraphQLSchema& SchemaBuilder::getSchema(bool forceRefresh) {
  if (!cachedSchema_.has_value() || forceRefresh) {
    cachedSchema_ = build();
  }
  return *cachedSchema_;
}

// ____________________________________________________________________________
std::vector<SchemaBuilder::DiscoveredType> SchemaBuilder::discoverTypes() {
  std::vector<DiscoveredType> types;

  // Query to discover classes with their instance counts
  // This query finds all distinct rdf:type values and counts their instances
  std::string query = R"(
    SELECT ?class (COUNT(?instance) AS ?count) WHERE {
      ?instance a ?class .
    }
    GROUP BY ?class
    HAVING (COUNT(?instance) > )" +
                      std::to_string(config_.minInstanceCount) + R"()
    ORDER BY DESC(?count)
    LIMIT )" + std::to_string(config_.maxTypes);

  auto results = executeQuery(query);

  for (const auto& row : results) {
    if (row.size() >= 2) {
      DiscoveredType type;
      type.iri = row[0];
      try {
        type.instanceCount = std::stoull(row[1]);
      } catch (...) {
        type.instanceCount = 0;
      }
      type.label = getLabelForIri(type.iri);
      if (type.label.empty()) {
        type.label = iriToName(type.iri);
      }
      types.push_back(std::move(type));
    }
  }

  return types;
}

// ____________________________________________________________________________
std::vector<SchemaBuilder::DiscoveredProperty>
SchemaBuilder::discoverProperties(const std::string& typeIri) {
  std::vector<DiscoveredProperty> properties;

  // Query to discover properties used with instances of this type
  std::string query = R"(
    SELECT ?prop (COUNT(?prop) AS ?count) (SAMPLE(?obj) AS ?sampleObj) WHERE {
      ?instance a <)" +
                      typeIri + R"(> .
      ?instance ?prop ?obj .
      FILTER(?prop != <http://www.w3.org/1999/02/22-rdf-syntax-ns#type>)
    }
    GROUP BY ?prop
    HAVING (COUNT(?prop) > )" +
                      std::to_string(config_.minPropertyUses) + R"()
    ORDER BY DESC(?count)
    LIMIT )" + std::to_string(config_.maxPropertiesPerType);

  auto results = executeQuery(query);

  for (const auto& row : results) {
    if (row.size() >= 2) {
      DiscoveredProperty prop;
      prop.iri = row[0];
      try {
        prop.useCount = std::stoull(row[1]);
      } catch (...) {
        prop.useCount = 0;
      }
      prop.label = getLabelForIri(prop.iri);
      if (prop.label.empty()) {
        prop.label = iriToName(prop.iri);
      }

      // Try to determine the range type from sample
      if (row.size() >= 3) {
        const std::string& sample = row[2];
        // Check if it looks like an IRI (object property)
        if (sample.front() == '<' ||
            (sample.find(':') != std::string::npos &&
             sample.find('"') == std::string::npos)) {
          prop.isObjectProperty = true;
          // Try to find the type of the object
          // For now, just mark as object property
        } else {
          prop.isObjectProperty = false;
          // Determine scalar type from the literal
          if (sample.find("^^") != std::string::npos) {
            size_t typePos = sample.find("^^");
            prop.rangeType = sample.substr(typePos + 2);
          } else {
            prop.rangeType = "http://www.w3.org/2001/XMLSchema#string";
          }
        }
      }

      properties.push_back(std::move(prop));
    }
  }

  return properties;
}

// ____________________________________________________________________________
void SchemaBuilder::buildSchemaTypes(
    const std::vector<DiscoveredType>& types,
    GraphQLSchema& schema) {
  // First pass: create all type stubs
  std::unordered_set<std::string> knownTypeIris;
  for (const auto& type : types) {
    knownTypeIris.insert(type.iri);
  }

  // Second pass: build full types with fields
  for (const auto& type : types) {
    SchemaType schemaType(type.label, type.iri);
    if (!type.label.empty()) {
      schemaType.label = type.label;
    }

    // Discover and add properties as fields
    auto properties = discoverProperties(type.iri);
    for (const auto& prop : properties) {
      auto field = propertyToField(prop, knownTypeIris);
      schemaType.addField(std::move(field));
    }

    schema.addType(std::move(schemaType));
  }
}

// ____________________________________________________________________________
SchemaField SchemaBuilder::propertyToField(
    const DiscoveredProperty& prop,
    const std::unordered_set<std::string>& knownTypeIris) {
  // Make name GraphQL-safe
  // Replace non-alphanumeric characters
  std::string safeName;
  for (char c : prop.label) {
    if (std::isalnum(c)) {
      safeName += c;
    } else if (c == ' ' || c == '-' || c == '_') {
      // CamelCase the next character
      if (!safeName.empty()) {
        safeName += '_';
      }
    }
  }
  if (!safeName.empty() && std::isdigit(safeName[0])) {
    safeName = "_" + safeName;
  }
  if (safeName.empty()) {
    safeName = "field_" + std::to_string(std::hash<std::string>{}(prop.iri) % 10000);
  }

  SchemaField field;
  if (prop.isObjectProperty) {
    // Object property - link to another type
    // Check if the range type is a known type in our schema
    std::string objectTypeName = "UnknownType";
    if (knownTypeIris.count(prop.rangeType) > 0) {
      objectTypeName = iriToName(prop.rangeType);
    }
    field = SchemaField(safeName, prop.iri, objectTypeName, true, false);
  } else {
    // Scalar property
    field = SchemaField(safeName, prop.iri, xsdToScalar(prop.rangeType), true, false);
  }

  if (!prop.label.empty()) {
    field.label = prop.label;
  }

  return field;
}

// ____________________________________________________________________________
ScalarType SchemaBuilder::xsdToScalar(const std::string& xsdType) {
  // Map XSD datatypes to GraphQL scalars
  static const std::unordered_map<std::string, ScalarType> xsdMap = {
      {"http://www.w3.org/2001/XMLSchema#string", ScalarType::STRING},
      {"http://www.w3.org/2001/XMLSchema#integer", ScalarType::INT},
      {"http://www.w3.org/2001/XMLSchema#int", ScalarType::INT},
      {"http://www.w3.org/2001/XMLSchema#long", ScalarType::INT},
      {"http://www.w3.org/2001/XMLSchema#short", ScalarType::INT},
      {"http://www.w3.org/2001/XMLSchema#byte", ScalarType::INT},
      {"http://www.w3.org/2001/XMLSchema#nonNegativeInteger", ScalarType::INT},
      {"http://www.w3.org/2001/XMLSchema#positiveInteger", ScalarType::INT},
      {"http://www.w3.org/2001/XMLSchema#decimal", ScalarType::FLOAT},
      {"http://www.w3.org/2001/XMLSchema#float", ScalarType::FLOAT},
      {"http://www.w3.org/2001/XMLSchema#double", ScalarType::FLOAT},
      {"http://www.w3.org/2001/XMLSchema#boolean", ScalarType::BOOLEAN},
      {"http://www.w3.org/2001/XMLSchema#date", ScalarType::DATE},
      {"http://www.w3.org/2001/XMLSchema#dateTime", ScalarType::DATETIME},
      {"http://www.w3.org/2001/XMLSchema#time", ScalarType::TIME},
      {"http://www.w3.org/2001/XMLSchema#gYear", ScalarType::INT},
      {"http://www.w3.org/2001/XMLSchema#anyURI", ScalarType::STRING},
  };

  // Clean up the type IRI
  std::string cleanType = xsdType;
  if (!cleanType.empty() && cleanType.front() == '<') {
    cleanType = cleanType.substr(1);
  }
  if (!cleanType.empty() && cleanType.back() == '>') {
    cleanType.pop_back();
  }

  auto it = xsdMap.find(cleanType);
  if (it != xsdMap.end()) {
    return it->second;
  }

  // Default to string
  return ScalarType::STRING;
}

// ____________________________________________________________________________
std::string SchemaBuilder::iriToName(const std::string& iri) {
  std::string cleanIri = iri;

  // Remove angle brackets
  if (cleanIri.front() == '<') {
    cleanIri = cleanIri.substr(1);
  }
  if (cleanIri.back() == '>') {
    cleanIri.pop_back();
  }

  // Find the local name (after last # or /)
  size_t pos = cleanIri.rfind('#');
  if (pos == std::string::npos) {
    pos = cleanIri.rfind('/');
  }

  std::string localName;
  if (pos != std::string::npos && pos + 1 < cleanIri.size()) {
    localName = cleanIri.substr(pos + 1);
  } else {
    localName = cleanIri;
  }

  // Make it a valid GraphQL name
  std::string result;
  for (char c : localName) {
    if (std::isalnum(c) || c == '_') {
      result += c;
    }
  }

  // Ensure it starts with a letter
  if (!result.empty() && !std::isalpha(result[0])) {
    result = "Type_" + result;
  }

  return result.empty() ? "Unknown" : result;
}

// ____________________________________________________________________________
std::string SchemaBuilder::getLabelForIri(const std::string& iri) {
  if (!config_.useLabelsAsFieldNames) {
    return "";
  }

  std::string cleanIri = iri;
  if (cleanIri.front() != '<') {
    cleanIri = "<" + cleanIri + ">";
  }

  // Query for label
  std::string query = R"(
    SELECT ?label WHERE {
      )" + cleanIri +
                      R"( <http://www.w3.org/2000/01/rdf-schema#label> ?label .
      FILTER(LANG(?label) = ")" +
                      config_.preferredLanguage + R"(" || LANG(?label) = "")
    }
    LIMIT 1
  )";

  auto results = executeQuery(query);
  if (!results.empty() && !results[0].empty()) {
    std::string label = results[0][0];
    // Remove quotes and language tag
    if (label.front() == '"') {
      size_t endQuote = label.rfind('"');
      if (endQuote > 0) {
        label = label.substr(1, endQuote - 1);
      }
    }
    return label;
  }

  return "";
}

// ____________________________________________________________________________
std::vector<std::vector<std::string>> SchemaBuilder::executeQuery(
    const std::string& sparql) {
  std::vector<std::vector<std::string>> results;

  try {
    // Parse the SPARQL query
    auto parsedQuery = SparqlParser::parseQuery(
        &index_.encodedIriManager(), sparql, {});

    // Create a query execution context
    // Note: We use nullptr for cache since this is for schema discovery,
    // not user queries. A simple allocator is created with limited memory.
    ad_utility::AllocatorWithLimit<Id> allocator{
        ad_utility::makeAllocationMemoryLeftThreadsafeObject(
            ad_utility::MemorySize::gigabytes(1))};

    // Create the QueryExecutionContext with all required parameters
    // nullptr for caches since we don't need caching for schema discovery
    QueryExecutionContext qec(
        index_,
        nullptr,  // QueryResultCache* - not needed for schema discovery
        std::move(allocator),
        SortPerformanceEstimator{},
        nullptr,  // NamedResultCache* - not needed
        nullptr   // MaterializedViewsManager* - not needed
    );

    // Create a cancellation handle
    auto cancellationHandle =
        std::make_shared<ad_utility::CancellationHandle<>>();

    // Plan and execute the query
    QueryPlanner qp(&qec, cancellationHandle);
    auto executionTree = qp.createExecutionTree(parsedQuery);

    // Get the result
    auto result = executionTree.getResult();
    if (!result) {
      return results;
    }

    const auto& idTable = result->idTable();
    const auto& localVocab = result->localVocab();
    const auto& varToCol = executionTree.getVariableColumns();

    // Extract column indices for SELECT variables
    std::vector<size_t> columnIndices;
    if (parsedQuery.hasSelectClause()) {
      const auto& selectClause = parsedQuery.selectClause();
      for (const auto& var : selectClause.getSelectedVariables()) {
        auto it = varToCol.find(var);
        if (it != varToCol.end()) {
          columnIndices.push_back(it->second.columnIndex_);
        }
      }
    } else {
      // If no select clause, use all columns
      for (size_t i = 0; i < idTable.numColumns(); ++i) {
        columnIndices.push_back(i);
      }
    }

    // Convert results to strings
    for (size_t rowIdx = 0; rowIdx < idTable.numRows(); ++rowIdx) {
      std::vector<std::string> row;
      for (size_t colIdx : columnIndices) {
        if (colIdx < idTable.numColumns()) {
          Id id = idTable(rowIdx, colIdx);
          auto stringAndType =
              ExportQueryExecutionTrees::idToStringAndType(
                  index_, id, localVocab);
          if (stringAndType.has_value()) {
            row.push_back(stringAndType->first);
          } else {
            row.push_back("");
          }
        } else {
          row.push_back("");
        }
      }
      results.push_back(std::move(row));
    }
  } catch (const std::exception& e) {
    // Log the error but don't propagate it - schema discovery should be
    // graceful
    AD_LOG_WARN << "Schema discovery query failed: " << e.what() << std::endl;
  }

  return results;
}

// ____________________________________________________________________________
bool SchemaBuilder::initializeOwlViews(bool forceRegenerate) {
  auto& generator = getOwlViewGenerator();

  // Analyze the ontology
  ontologyAnalysis_ = generator.analyzeOntology();

  if (ontologyAnalysis_->recommendedViews.empty()) {
    AD_LOG_INFO << "No OWL inference views recommended for this dataset."
                << std::endl;
    return false;
  }

  // Check which views need to be created
  std::vector<ViewConfig> viewsToCreate;
  for (const auto& viewConfig : ontologyAnalysis_->recommendedViews) {
    if (forceRegenerate || !generator.viewExists(viewConfig.name)) {
      viewsToCreate.push_back(viewConfig);
    }
  }

  if (viewsToCreate.empty()) {
    AD_LOG_INFO << "All OWL inference views already exist." << std::endl;
    return true;
  }

  // Only create views if auto-generation is enabled
  if (!config_.autoGenerateOwlViews) {
    AD_LOG_INFO << viewsToCreate.size()
                << " OWL inference views could be created. "
                << "Set autoGenerateOwlViews=true to generate them."
                << std::endl;
    return false;
  }

  AD_LOG_INFO << "Generating " << viewsToCreate.size()
              << " OWL inference views..." << std::endl;

  // Create each view
  for (const auto& viewConfig : viewsToCreate) {
    try {
      AD_LOG_INFO << "  Creating view: " << viewConfig.name << std::endl;

      // Parse the query
      auto parsedQuery = SparqlParser::parseQuery(&index_.encodedIriManager(),
                                                  viewConfig.query, {});

      // Create allocator for view writing
      ad_utility::AllocatorWithLimit<Id> allocator{
          ad_utility::makeAllocationMemoryLeftThreadsafeObject(
              ad_utility::MemorySize::gigabytes(1))};

      // Create query execution context
      QueryExecutionContext qec(index_, nullptr, std::move(allocator),
                                SortPerformanceEstimator{}, nullptr, nullptr);

      auto cancellationHandle =
          std::make_shared<ad_utility::CancellationHandle<>>();

      // Plan the query
      QueryPlanner qp(&qec, cancellationHandle);
      auto executionTree =
          std::make_shared<QueryExecutionTree>(qp.createExecutionTree(parsedQuery));

      // Create the query plan (tuple: QET, QEC, ParsedQuery)
      auto qecPtr = std::make_shared<QueryExecutionContext>(std::move(qec));
      qlever::QueryPlan plan = std::make_tuple(
          executionTree,
          qecPtr,
          std::move(parsedQuery));

      // Write the view to disk
      MaterializedViewWriter::writeViewToDisk(
          index_.getTextName(), viewConfig.name, plan,
          ad_utility::MemorySize::megabytes(config_.owlViewConfig.viewMemoryLimitMB));

      AD_LOG_INFO << "  View created successfully: " << viewConfig.name
                  << std::endl;
    } catch (const std::exception& e) {
      AD_LOG_WARN << "  Failed to create view " << viewConfig.name << ": "
                  << e.what() << std::endl;
    }
  }

  return true;
}

// ____________________________________________________________________________
OwlViewGenerator& SchemaBuilder::getOwlViewGenerator() {
  if (!owlViewGenerator_) {
    owlViewGenerator_ =
        std::make_unique<OwlViewGenerator>(index_, config_.owlViewConfig);
  }
  return *owlViewGenerator_;
}

// ____________________________________________________________________________
bool SchemaBuilder::hasOwlViews() const {
  if (!ontologyAnalysis_.has_value()) {
    return false;
  }

  // Check if at least the required views exist
  for (const auto& viewConfig : ontologyAnalysis_->recommendedViews) {
    if (viewConfig.required) {
      std::string viewFile = index_.getTextName() + ".view." + viewConfig.name +
                             ".index.spo";
      if (!std::filesystem::exists(viewFile)) {
        return false;
      }
    }
  }
  return true;
}

// ____________________________________________________________________________
const OntologyAnalysis* SchemaBuilder::getOntologyAnalysis() const {
  if (ontologyAnalysis_.has_value()) {
    return &*ontologyAnalysis_;
  }
  return nullptr;
}

// ____________________________________________________________________________
std::vector<SchemaBuilder::DiscoveredType>
SchemaBuilder::discoverTypesWithOwl() {
  std::vector<DiscoveredType> types;

  // If we have the inferred-types view, use it for better type discovery
  if (hasOwlViews()) {
    // Query using the materialized view
    // The view has columns: instance, class, depth
    std::string query = R"(
      SELECT ?class (COUNT(DISTINCT ?instance) AS ?count) WHERE {
        SERVICE <https://qlever.cs.uni-freiburg.de/materializedView/)" +
                        std::string(ViewNames::INFERRED_TYPES) + R"(> {
          _:config <https://qlever.cs.uni-freiburg.de/materializedView/column-instance> ?instance ;
                   <https://qlever.cs.uni-freiburg.de/materializedView/column-class> ?class .
        }
      }
      GROUP BY ?class
      HAVING (COUNT(DISTINCT ?instance) > )" +
                        std::to_string(config_.minInstanceCount) + R"()
      ORDER BY DESC(?count)
      LIMIT )" + std::to_string(config_.maxTypes);

    auto results = executeQuery(query);

    for (const auto& row : results) {
      if (row.size() >= 2) {
        DiscoveredType type;
        type.iri = row[0];
        try {
          type.instanceCount = std::stoull(row[1]);
        } catch (...) {
          type.instanceCount = 0;
        }
        type.label = getLabelForIri(type.iri);
        if (type.label.empty()) {
          type.label = iriToName(type.iri);
        }
        types.push_back(std::move(type));
      }
    }

    if (!types.empty()) {
      AD_LOG_INFO << "Discovered " << types.size()
                  << " types using OWL inference view." << std::endl;
      return types;
    }
  }

  // Fall back to standard discovery
  return discoverTypes();
}

// ____________________________________________________________________________
std::vector<SchemaBuilder::DiscoveredProperty>
SchemaBuilder::discoverPropertiesWithOwl(const std::string& typeIri) {
  // Start with direct properties
  auto properties = discoverProperties(typeIri);

  // If inheritance is enabled and we have views, add inherited properties
  if (config_.inheritProperties && hasOwlViews()) {
    auto inherited = getInheritedProperties(typeIri);
    for (auto& prop : inherited) {
      // Check if property already exists
      bool exists = false;
      for (const auto& existing : properties) {
        if (existing.iri == prop.iri) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        properties.push_back(std::move(prop));
      }
    }
  }

  return properties;
}

// ____________________________________________________________________________
void SchemaBuilder::buildInterfacesFromOwl([[maybe_unused]] GraphQLSchema& schema) {
  if (!ontologyAnalysis_.has_value() ||
      ontologyAnalysis_->potentialInterfaces.empty()) {
    return;
  }

  AD_LOG_INFO << "Building GraphQL interfaces from "
              << ontologyAnalysis_->potentialInterfaces.size()
              << " potential superclasses..." << std::endl;

  // For each potential interface, create a GraphQL interface type
  for (const auto& interfaceIri : ontologyAnalysis_->potentialInterfaces) {
    std::string name = iriToName(interfaceIri);

    // Discover properties for this interface
    auto properties = discoverProperties(interfaceIri);

    if (properties.empty()) {
      continue;  // Skip interfaces with no properties
    }

    // Create the interface
    // Note: In GraphQL, interfaces define a set of fields that implementing
    // types must include. We model this by adding the interface to the schema
    // and updating implementing types.

    // For now, we'll add interface info to the schema metadata
    // A full implementation would modify GraphQLSchema to support interfaces

    AD_LOG_INFO << "  Interface: " << name << " with " << properties.size()
                << " properties" << std::endl;

    // Find types that implement this interface (subclasses)
    std::string query = R"(
      SELECT DISTINCT ?subclass WHERE {
        ?subclass <http://www.w3.org/2000/01/rdf-schema#subClassOf> <)" +
                        interfaceIri + R"(> .
        FILTER(!isBlank(?subclass))
      }
    )";

    auto results = executeQuery(query);
    for (const auto& row : results) {
      if (!row.empty()) {
        std::string subclassIri = row[0];
        std::string subclassName = iriToName(subclassIri);
        AD_LOG_DEBUG << "    Implementing type: " << subclassName << std::endl;

        // Find the implementing type in the schema and record the interface
        SchemaType* implementingType = schema.findTypeMutable(subclassName);
        if (implementingType) {
          // Add interface to the type's implements list (avoid duplicates)
          auto& impls = implementingType->implementsInterfaces;
          if (std::find(impls.begin(), impls.end(), name) == impls.end()) {
            impls.push_back(name);
          }
        }
      }
    }
  }
}

// ____________________________________________________________________________
std::vector<SchemaBuilder::DiscoveredProperty>
SchemaBuilder::getInheritedProperties(const std::string& typeIri) {
  std::vector<DiscoveredProperty> inherited;

  // Query for superclasses of this type
  std::string query = R"(
    SELECT DISTINCT ?superclass WHERE {
      <)" + typeIri +
                      R"(> <http://www.w3.org/2000/01/rdf-schema#subClassOf>+ ?superclass .
      FILTER(!isBlank(?superclass))
    }
  )";

  auto results = executeQuery(query);
  for (const auto& row : results) {
    if (!row.empty()) {
      std::string superclassIri = row[0];
      // Get properties from this superclass
      auto superProps = discoverProperties(superclassIri);
      for (auto& prop : superProps) {
        // Mark as inherited
        inherited.push_back(std::move(prop));
      }
    }
  }

  return inherited;
}

// ____________________________________________________________________________
std::vector<std::vector<std::string>> SchemaBuilder::executeViewQuery(
    const std::string& viewName, const std::string& query) {
  // Check if the view exists
  std::string viewFile =
      index_.getTextName() + ".view." + viewName + ".index.spo";
  if (!std::filesystem::exists(viewFile)) {
    AD_LOG_WARN << "Materialized view does not exist: " << viewName
                << std::endl;
    return {};
  }

  // Execute the query (which should reference the view via SERVICE syntax)
  return executeQuery(query);
}

}  // namespace graphql

#endif  // QLEVER_GRAPHQL_SUPPORT
