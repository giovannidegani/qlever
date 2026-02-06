// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifndef QLEVER_SRC_ENGINE_GRAPHQL_OWLVIEWGENERATOR_H
#define QLEVER_SRC_ENGINE_GRAPHQL_OWLVIEWGENERATOR_H

#ifdef QLEVER_GRAPHQL_SUPPORT

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "index/Index.h"

namespace graphql {

/// Configuration for a single materialized view
struct ViewConfig {
  /// Name of the view (alphanumeric + hyphens only)
  std::string name;

  /// SPARQL SELECT query to materialize
  std::string query;

  /// Whether this view is required for basic functionality
  /// (vs. optional optimization)
  bool required = false;

  /// Human-readable description of what this view provides
  std::string description;

  /// Estimated cost factor (higher = more expensive to compute)
  double costFactor = 1.0;
};

/// Configuration options for OWL view generation
struct OwlViewGeneratorConfig {
  /// Maximum depth for transitive closure computations
  /// (e.g., rdfs:subClassOf chains)
  size_t maxTransitiveDepth = 10;

  /// Whether to generate subClassOf inference view
  bool enableSubClassInference = true;

  /// Whether to generate subPropertyOf inference view
  bool enableSubPropertyInference = true;

  /// Whether to generate owl:inverseOf view
  bool enableInverseProperties = false;

  /// Whether to generate owl:sameAs unification view
  bool enableSameAsUnification = false;

  /// Whether to generate property domain/range validation view
  bool enableDomainRangeValidation = false;

  /// Whether to generate interface membership view (for GraphQL interfaces)
  bool enableInterfaceMembership = true;

  /// Minimum number of class hierarchy relationships to justify creating a view
  size_t minSubClassRelationships = 10;

  /// Memory limit for view computation (sorting)
  size_t viewMemoryLimitMB = 4096;
};

/// Result of ontology analysis
struct OntologyAnalysis {
  /// Number of rdfs:subClassOf relationships found
  size_t subClassCount = 0;

  /// Number of rdfs:subPropertyOf relationships found
  size_t subPropertyCount = 0;

  /// Number of owl:inverseOf relationships found
  size_t inversePropertyCount = 0;

  /// Number of owl:sameAs relationships found
  size_t sameAsCount = 0;

  /// Number of rdfs:domain declarations found
  size_t domainCount = 0;

  /// Number of rdfs:range declarations found
  size_t rangeCount = 0;

  /// Maximum depth of class hierarchy
  size_t maxHierarchyDepth = 0;

  /// Set of class IRIs that are parents of multiple classes (potential interfaces)
  std::unordered_set<std::string> potentialInterfaces;

  /// Recommended views based on analysis
  std::vector<ViewConfig> recommendedViews;
};

/// View names as constants for use in queries
struct ViewNames {
  static constexpr const char* INFERRED_TYPES = "owl-inferred-types";
  static constexpr const char* INFERRED_PROPERTIES = "owl-inferred-properties";
  static constexpr const char* INVERSE_PROPERTIES = "owl-inverse-properties";
  static constexpr const char* UNIFIED_ENTITIES = "owl-unified-entities";
  static constexpr const char* INTERFACE_MEMBERSHIP = "owl-interface-membership";
  static constexpr const char* INSTANCE_PROPERTIES = "owl-instance-properties";
  static constexpr const char* PROPERTY_VALIDATION = "owl-property-validation";
};

/// Generates materialized views for OWL/RDFS inference to optimize
/// GraphQL schema building and query execution.
///
/// This class analyzes the ontology in a QLever index and generates
/// SPARQL queries that can be materialized as views. These views
/// pre-compute inference results like:
/// - rdfs:subClassOf transitive closure
/// - rdfs:subPropertyOf transitive closure
/// - owl:inverseOf expansions
/// - owl:sameAs identity unification
///
/// Usage:
///   OwlViewGenerator generator(index, config);
///   auto analysis = generator.analyzeOntology();
///   for (const auto& view : analysis.recommendedViews) {
///     // Use MaterializedViewWriter to create the view
///   }
class OwlViewGenerator {
 public:
  /// Constructor
  /// @param index The QLever index to analyze
  /// @param config Configuration options
  explicit OwlViewGenerator(const Index& index,
                            OwlViewGeneratorConfig config = {});

  /// Analyze the ontology and determine which views would be beneficial
  /// @return Analysis results including recommended views
  OntologyAnalysis analyzeOntology();

  /// Generate all recommended views based on analysis
  /// @return List of view configurations to create
  std::vector<ViewConfig> generateViewConfigs();

  /// Generate a specific view configuration
  /// @param viewName One of the ViewNames constants
  /// @return View configuration, or nullopt if not applicable
  std::optional<ViewConfig> generateViewConfig(const std::string& viewName);

  /// Check if a view already exists on disk
  /// @param viewName The view name to check
  /// @return True if the view files exist
  bool viewExists(const std::string& viewName) const;

  /// Get the current configuration
  const OwlViewGeneratorConfig& config() const { return config_; }

 private:
  const Index& index_;
  OwlViewGeneratorConfig config_;
  std::optional<OntologyAnalysis> cachedAnalysis_;

  /// Execute a SPARQL query and get results
  std::vector<std::vector<std::string>> executeQuery(const std::string& sparql);

  /// Count relationships of a specific type
  size_t countRelationships(const std::string& predicate);

  /// Check if the ontology uses a specific OWL/RDFS feature
  bool hasSubClassHierarchy();
  bool hasSubPropertyHierarchy();
  bool hasInverseProperties();
  bool hasSameAsLinks();
  bool hasDomainRangeDeclarations();

  /// Find classes that are superclasses of multiple other classes
  std::unordered_set<std::string> findPotentialInterfaces();

  /// Compute maximum depth of class hierarchy
  size_t computeMaxHierarchyDepth();

  /// Generate specific view queries
  ViewConfig generateSubClassView();
  ViewConfig generateSubPropertyView();
  ViewConfig generateInversePropertyView();
  ViewConfig generateSameAsView();
  ViewConfig generateInterfaceMembershipView();
  ViewConfig generateInstancePropertiesView();
  ViewConfig generatePropertyValidationView();

  /// Build transitive closure query for a given property
  /// @param property The property IRI (e.g., rdfs:subClassOf)
  /// @param maxDepth Maximum depth to expand
  /// @return SPARQL query that computes transitive closure
  std::string buildTransitiveClosureQuery(const std::string& property,
                                          size_t maxDepth);
};

}  // namespace graphql

#endif  // QLEVER_GRAPHQL_SUPPORT
#endif  // QLEVER_SRC_ENGINE_GRAPHQL_OWLVIEWGENERATOR_H
