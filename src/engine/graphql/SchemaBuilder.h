// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifndef QLEVER_SRC_ENGINE_GRAPHQL_SCHEMABUILDER_H
#define QLEVER_SRC_ENGINE_GRAPHQL_SCHEMABUILDER_H

#ifdef QLEVER_GRAPHQL_SUPPORT

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/graphql/OwlViewGenerator.h"
#include "index/Index.h"
#include "parser/graphql/GraphQLSchema.h"

namespace graphql {

/// Configuration options for schema building
struct SchemaBuilderConfig {
  /// Minimum number of instances for a class to be included as a type
  size_t minInstanceCount = 100;

  /// Maximum number of types to include in the schema
  size_t maxTypes = 500;

  /// Minimum number of uses for a property to be included
  size_t minPropertyUses = 10;

  /// Maximum number of properties per type
  size_t maxPropertiesPerType = 100;

  /// Whether to use rdfs:label for field names
  bool useLabelsAsFieldNames = true;

  /// Preferred language for labels (e.g., "en")
  std::string preferredLanguage = "en";

  /// Whether to discover relationships between types
  bool discoverRelationships = true;

  /// Namespace prefixes to use for shortening IRIs
  std::unordered_map<std::string, std::string> prefixes = {
      {"http://www.w3.org/1999/02/22-rdf-syntax-ns#", "rdf"},
      {"http://www.w3.org/2000/01/rdf-schema#", "rdfs"},
      {"http://www.w3.org/2001/XMLSchema#", "xsd"},
      {"http://schema.org/", "schema"},
      {"http://www.wikidata.org/entity/", "wd"},
      {"http://www.wikidata.org/prop/direct/", "wdt"},
  };

  // ---- OWL/RDFS Inference Options ----

  /// Whether to use OWL inference views if available
  bool useOwlInference = true;

  /// Whether to automatically generate OWL inference views if missing
  bool autoGenerateOwlViews = false;

  /// Configuration for OWL view generation
  OwlViewGeneratorConfig owlViewConfig;

  /// Whether to generate GraphQL interfaces from superclasses
  bool generateInterfaces = true;

  /// Whether to inherit properties from superclasses
  bool inheritProperties = true;
};

/// Builds a GraphQL schema from RDF data in a QLever index.
///
/// This class introspects the RDF data to discover:
/// - Classes (rdf:type values) that become GraphQL types
/// - Properties used with those classes that become fields
/// - Relationships between types (object properties)
/// - Datatypes for scalar fields
///
/// The schema can be cached and refreshed periodically for performance.
class SchemaBuilder {
 public:
  /// Constructor
  /// @param index The QLever index to introspect
  /// @param config Configuration options
  explicit SchemaBuilder(const Index& index,
                         SchemaBuilderConfig config = {});

  /// Build the GraphQL schema from the index
  /// @return The generated schema
  GraphQLSchema build();

  /// Build schema asynchronously (returns cached if available)
  /// @param forceRefresh If true, rebuilds even if cached
  /// @return The generated or cached schema
  const GraphQLSchema& getSchema(bool forceRefresh = false);

  /// Check if the schema is cached
  bool hasCachedSchema() const { return cachedSchema_.has_value(); }

  /// Clear the cached schema
  void clearCache() { cachedSchema_.reset(); }

  /// Initialize OWL inference views
  /// @param forceRegenerate If true, regenerate views even if they exist
  /// @return True if views were successfully initialized
  bool initializeOwlViews(bool forceRegenerate = false);

  /// Get the OWL view generator (creates one if needed)
  OwlViewGenerator& getOwlViewGenerator();

  /// Check if OWL inference views are available
  bool hasOwlViews() const;

  /// Get the ontology analysis results
  const OntologyAnalysis* getOntologyAnalysis() const;

 private:
  const Index& index_;
  SchemaBuilderConfig config_;
  std::optional<GraphQLSchema> cachedSchema_;
  std::unique_ptr<OwlViewGenerator> owlViewGenerator_;
  std::optional<OntologyAnalysis> ontologyAnalysis_;

  /// Discovered type information
  struct DiscoveredType {
    std::string iri;
    std::string label;
    size_t instanceCount;
    std::unordered_map<std::string, size_t> properties;  // IRI -> count
  };

  /// Discovered property information
  struct DiscoveredProperty {
    std::string iri;
    std::string label;
    std::string rangeType;  // XSD type or class IRI
    bool isObjectProperty;
    size_t useCount;
  };

  /// Discover classes from the index
  std::vector<DiscoveredType> discoverTypes();

  /// Discover properties for a type
  std::vector<DiscoveredProperty> discoverProperties(
      const std::string& typeIri);

  /// Convert discovered types to GraphQL schema
  void buildSchemaTypes(
      const std::vector<DiscoveredType>& types,
      GraphQLSchema& schema);

  /// Convert a property to a schema field
  SchemaField propertyToField(
      const DiscoveredProperty& prop,
      const std::unordered_set<std::string>& knownTypeIris);

  /// Convert XSD datatype to GraphQL scalar
  ScalarType xsdToScalar(const std::string& xsdType);

  /// Convert an IRI to a GraphQL-safe name
  std::string iriToName(const std::string& iri);

  /// Get label for an IRI from the index
  std::string getLabelForIri(const std::string& iri);

  /// Execute a SPARQL query and get results as strings
  /// This is a simplified interface for introspection queries
  std::vector<std::vector<std::string>> executeQuery(
      const std::string& sparql);

  // ---- OWL-enhanced schema building methods ----

  /// Discover types using OWL inference views
  std::vector<DiscoveredType> discoverTypesWithOwl();

  /// Discover properties for a type using OWL inheritance
  std::vector<DiscoveredProperty> discoverPropertiesWithOwl(
      const std::string& typeIri);

  /// Build GraphQL interfaces from OWL class hierarchy
  void buildInterfacesFromOwl(GraphQLSchema& schema);

  /// Get inherited properties for a type from the class hierarchy
  std::vector<DiscoveredProperty> getInheritedProperties(
      const std::string& typeIri);

  /// Execute a query against a materialized view
  std::vector<std::vector<std::string>> executeViewQuery(
      const std::string& viewName, const std::string& query);
};

}  // namespace graphql

#endif  // QLEVER_GRAPHQL_SUPPORT
#endif  // QLEVER_SRC_ENGINE_GRAPHQL_SCHEMABUILDER_H
