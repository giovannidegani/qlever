// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifdef QLEVER_GRAPHQL_SUPPORT

#include "engine/graphql/OwlViewGenerator.h"

#include <filesystem>
#include <sstream>

#include "engine/ExportQueryExecutionTrees.h"
#include "engine/QueryExecutionContext.h"
#include "engine/QueryPlanner.h"
#include "engine/SortPerformanceEstimator.h"
#include "parser/SparqlParser.h"
#include "util/CancellationHandle.h"
#include "util/Log.h"

namespace graphql {

// ____________________________________________________________________________
OwlViewGenerator::OwlViewGenerator(const Index& index,
                                   OwlViewGeneratorConfig config)
    : index_(index), config_(std::move(config)) {}

// ____________________________________________________________________________
OntologyAnalysis OwlViewGenerator::analyzeOntology() {
  if (cachedAnalysis_.has_value()) {
    return *cachedAnalysis_;
  }

  OntologyAnalysis analysis;

  AD_LOG_INFO << "Analyzing ontology for OWL inference views..." << std::endl;

  // Count various relationship types
  analysis.subClassCount =
      countRelationships("<http://www.w3.org/2000/01/rdf-schema#subClassOf>");
  analysis.subPropertyCount =
      countRelationships("<http://www.w3.org/2000/01/rdf-schema#subPropertyOf>");
  analysis.inversePropertyCount =
      countRelationships("<http://www.w3.org/2002/07/owl#inverseOf>");
  analysis.sameAsCount =
      countRelationships("<http://www.w3.org/2002/07/owl#sameAs>");
  analysis.domainCount =
      countRelationships("<http://www.w3.org/2000/01/rdf-schema#domain>");
  analysis.rangeCount =
      countRelationships("<http://www.w3.org/2000/01/rdf-schema#range>");

  AD_LOG_INFO << "Ontology analysis results:" << std::endl;
  AD_LOG_INFO << "  rdfs:subClassOf: " << analysis.subClassCount << std::endl;
  AD_LOG_INFO << "  rdfs:subPropertyOf: " << analysis.subPropertyCount << std::endl;
  AD_LOG_INFO << "  owl:inverseOf: " << analysis.inversePropertyCount << std::endl;
  AD_LOG_INFO << "  owl:sameAs: " << analysis.sameAsCount << std::endl;
  AD_LOG_INFO << "  rdfs:domain: " << analysis.domainCount << std::endl;
  AD_LOG_INFO << "  rdfs:range: " << analysis.rangeCount << std::endl;

  // Find potential interfaces (classes with multiple subclasses)
  if (analysis.subClassCount >= config_.minSubClassRelationships) {
    analysis.potentialInterfaces = findPotentialInterfaces();
    analysis.maxHierarchyDepth = computeMaxHierarchyDepth();
    AD_LOG_INFO << "  Potential interfaces: "
                << analysis.potentialInterfaces.size() << std::endl;
    AD_LOG_INFO << "  Max hierarchy depth: " << analysis.maxHierarchyDepth
                << std::endl;
  }

  // Generate recommended views based on analysis
  if (config_.enableSubClassInference &&
      analysis.subClassCount >= config_.minSubClassRelationships) {
    analysis.recommendedViews.push_back(generateSubClassView());
  }

  if (config_.enableSubPropertyInference && analysis.subPropertyCount > 0) {
    analysis.recommendedViews.push_back(generateSubPropertyView());
  }

  if (config_.enableInverseProperties && analysis.inversePropertyCount > 0) {
    analysis.recommendedViews.push_back(generateInversePropertyView());
  }

  if (config_.enableSameAsUnification && analysis.sameAsCount > 0) {
    analysis.recommendedViews.push_back(generateSameAsView());
  }

  if (config_.enableInterfaceMembership &&
      !analysis.potentialInterfaces.empty()) {
    analysis.recommendedViews.push_back(generateInterfaceMembershipView());
  }

  if (config_.enableDomainRangeValidation &&
      (analysis.domainCount > 0 || analysis.rangeCount > 0)) {
    analysis.recommendedViews.push_back(generatePropertyValidationView());
  }

  // Always recommend instance properties view if there's a class hierarchy
  if (analysis.subClassCount >= config_.minSubClassRelationships) {
    analysis.recommendedViews.push_back(generateInstancePropertiesView());
  }

  AD_LOG_INFO << "Recommended " << analysis.recommendedViews.size()
              << " materialized views for OWL inference." << std::endl;

  cachedAnalysis_ = analysis;
  return analysis;
}

// ____________________________________________________________________________
std::vector<ViewConfig> OwlViewGenerator::generateViewConfigs() {
  auto analysis = analyzeOntology();
  return analysis.recommendedViews;
}

// ____________________________________________________________________________
std::optional<ViewConfig> OwlViewGenerator::generateViewConfig(
    const std::string& viewName) {
  if (viewName == ViewNames::INFERRED_TYPES) {
    if (hasSubClassHierarchy()) {
      return generateSubClassView();
    }
  } else if (viewName == ViewNames::INFERRED_PROPERTIES) {
    if (hasSubPropertyHierarchy()) {
      return generateSubPropertyView();
    }
  } else if (viewName == ViewNames::INVERSE_PROPERTIES) {
    if (hasInverseProperties()) {
      return generateInversePropertyView();
    }
  } else if (viewName == ViewNames::UNIFIED_ENTITIES) {
    if (hasSameAsLinks()) {
      return generateSameAsView();
    }
  } else if (viewName == ViewNames::INTERFACE_MEMBERSHIP) {
    auto interfaces = findPotentialInterfaces();
    if (!interfaces.empty()) {
      return generateInterfaceMembershipView();
    }
  } else if (viewName == ViewNames::INSTANCE_PROPERTIES) {
    if (hasSubClassHierarchy()) {
      return generateInstancePropertiesView();
    }
  } else if (viewName == ViewNames::PROPERTY_VALIDATION) {
    if (hasDomainRangeDeclarations()) {
      return generatePropertyValidationView();
    }
  }

  return std::nullopt;
}

// ____________________________________________________________________________
bool OwlViewGenerator::viewExists(const std::string& viewName) const {
  // Check if the view files exist on disk
  std::string basename = index_.getTextName();
  std::string viewFile = basename + ".view." + viewName + ".index.spo";
  return std::filesystem::exists(viewFile);
}

// ____________________________________________________________________________
std::vector<std::vector<std::string>> OwlViewGenerator::executeQuery(
    const std::string& sparql) {
  std::vector<std::vector<std::string>> results;

  try {
    auto parsedQuery =
        SparqlParser::parseQuery(&index_.encodedIriManager(), sparql, {});

    ad_utility::AllocatorWithLimit<Id> allocator{
        ad_utility::makeAllocationMemoryLeftThreadsafeObject(
            ad_utility::MemorySize::gigabytes(1))};

    QueryExecutionContext qec(index_, nullptr, std::move(allocator),
                              SortPerformanceEstimator{}, nullptr, nullptr);

    auto cancellationHandle =
        std::make_shared<ad_utility::CancellationHandle<>>();

    QueryPlanner qp(&qec, cancellationHandle);
    auto executionTree = qp.createExecutionTree(parsedQuery);

    auto result = executionTree.getResult();
    if (!result) {
      return results;
    }

    const auto& idTable = result->idTable();
    const auto& localVocab = result->localVocab();
    const auto& varToCol = executionTree.getVariableColumns();

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
      for (size_t i = 0; i < idTable.numColumns(); ++i) {
        columnIndices.push_back(i);
      }
    }

    for (size_t rowIdx = 0; rowIdx < idTable.numRows(); ++rowIdx) {
      std::vector<std::string> row;
      for (size_t colIdx : columnIndices) {
        if (colIdx < idTable.numColumns()) {
          Id id = idTable(rowIdx, colIdx);
          auto stringAndType = ExportQueryExecutionTrees::idToStringAndType(
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
    AD_LOG_WARN << "OWL analysis query failed: " << e.what() << std::endl;
  }

  return results;
}

// ____________________________________________________________________________
size_t OwlViewGenerator::countRelationships(const std::string& predicate) {
  std::string query =
      "SELECT (COUNT(*) AS ?count) WHERE { ?s " + predicate + " ?o }";

  auto results = executeQuery(query);
  if (!results.empty() && !results[0].empty()) {
    try {
      return std::stoull(results[0][0]);
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

// ____________________________________________________________________________
bool OwlViewGenerator::hasSubClassHierarchy() {
  return countRelationships(
             "<http://www.w3.org/2000/01/rdf-schema#subClassOf>") >=
         config_.minSubClassRelationships;
}

// ____________________________________________________________________________
bool OwlViewGenerator::hasSubPropertyHierarchy() {
  return countRelationships(
             "<http://www.w3.org/2000/01/rdf-schema#subPropertyOf>") > 0;
}

// ____________________________________________________________________________
bool OwlViewGenerator::hasInverseProperties() {
  return countRelationships("<http://www.w3.org/2002/07/owl#inverseOf>") > 0;
}

// ____________________________________________________________________________
bool OwlViewGenerator::hasSameAsLinks() {
  return countRelationships("<http://www.w3.org/2002/07/owl#sameAs>") > 0;
}

// ____________________________________________________________________________
bool OwlViewGenerator::hasDomainRangeDeclarations() {
  return countRelationships("<http://www.w3.org/2000/01/rdf-schema#domain>") >
             0 ||
         countRelationships("<http://www.w3.org/2000/01/rdf-schema#range>") > 0;
}

// ____________________________________________________________________________
std::unordered_set<std::string> OwlViewGenerator::findPotentialInterfaces() {
  std::unordered_set<std::string> interfaces;

  // Find classes that are parents of multiple other classes
  std::string query = R"(
    SELECT ?parent (COUNT(DISTINCT ?child) AS ?childCount) WHERE {
      ?child <http://www.w3.org/2000/01/rdf-schema#subClassOf> ?parent .
      FILTER(!isBlank(?parent))
    }
    GROUP BY ?parent
    HAVING(COUNT(DISTINCT ?child) > 1)
  )";

  auto results = executeQuery(query);
  for (const auto& row : results) {
    if (!row.empty()) {
      interfaces.insert(row[0]);
    }
  }

  return interfaces;
}

// ____________________________________________________________________________
size_t OwlViewGenerator::computeMaxHierarchyDepth() {
  // This is a simplified approximation - we check up to maxTransitiveDepth
  // levels and see how many levels actually have data
  size_t maxDepth = 0;

  for (size_t depth = 1; depth <= config_.maxTransitiveDepth; ++depth) {
    // Build a query that checks if depth-level inheritance exists
    std::ostringstream query;
    query << "SELECT (COUNT(*) AS ?count) WHERE { ?c0 ";
    for (size_t i = 0; i < depth; ++i) {
      if (i > 0) {
        query << " ?c" << i << " ";
      }
      query << "<http://www.w3.org/2000/01/rdf-schema#subClassOf>";
      if (i < depth - 1) {
        query << " ?c" << (i + 1) << " . ?c" << (i + 1);
      }
    }
    query << " ?cN } LIMIT 1";

    auto results = executeQuery(query.str());
    if (!results.empty() && !results[0].empty()) {
      try {
        size_t count = std::stoull(results[0][0]);
        if (count > 0) {
          maxDepth = depth;
        } else {
          break;  // No more levels
        }
      } catch (...) {
        break;
      }
    }
  }

  return maxDepth;
}

// ____________________________________________________________________________
std::string OwlViewGenerator::buildTransitiveClosureQuery(
    const std::string& property, size_t maxDepth) {
  // Build a UNION-based transitive closure query
  // This is necessary because QLever doesn't support property paths (*)
  std::ostringstream query;
  query << "SELECT ?start ?end ?depth WHERE {\n";

  for (size_t depth = 1; depth <= maxDepth; ++depth) {
    if (depth > 1) {
      query << "  UNION\n";
    }
    query << "  {\n";
    query << "    ?start";

    for (size_t i = 0; i < depth; ++i) {
      if (i > 0) {
        query << " ?mid" << i;
      }
      query << " " << property;
      if (i < depth - 1) {
        query << " ?mid" << (i + 1) << " .\n    ?mid" << (i + 1);
      }
    }
    query << " ?end .\n";
    query << "    BIND(" << depth << " AS ?depth)\n";
    query << "  }\n";
  }

  query << "}\n";
  return query.str();
}

// ____________________________________________________________________________
ViewConfig OwlViewGenerator::generateSubClassView() {
  ViewConfig config;
  config.name = ViewNames::INFERRED_TYPES;
  config.required = true;
  config.description =
      "Pre-computed rdfs:subClassOf transitive closure for type inference";
  config.costFactor = 2.0;

  // Generate query that expands all rdf:type relationships through
  // the subClassOf hierarchy
  std::ostringstream query;
  query << "SELECT ?instance ?class ?depth WHERE {\n";

  // Direct types (depth 0)
  query << "  {\n";
  query << "    ?instance <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> "
           "?class .\n";
  query << "    FILTER(!isBlank(?class))\n";
  query << "    BIND(0 AS ?depth)\n";
  query << "  }\n";

  // Inferred types through subClassOf hierarchy
  size_t maxDepth =
      std::min(config_.maxTransitiveDepth, static_cast<size_t>(10));
  for (size_t depth = 1; depth <= maxDepth; ++depth) {
    query << "  UNION\n";
    query << "  {\n";
    query << "    ?instance <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> "
             "?directClass .\n";
    query << "    ?directClass";

    for (size_t i = 0; i < depth; ++i) {
      if (i > 0) {
        query << " ?mid" << i;
      }
      query << " <http://www.w3.org/2000/01/rdf-schema#subClassOf>";
      if (i < depth - 1) {
        query << " ?mid" << (i + 1) << " .\n    ?mid" << (i + 1);
      }
    }
    query << " ?class .\n";
    query << "    FILTER(!isBlank(?class))\n";
    query << "    BIND(" << depth << " AS ?depth)\n";
    query << "  }\n";
  }

  query << "}\n";

  config.query = query.str();
  return config;
}

// ____________________________________________________________________________
ViewConfig OwlViewGenerator::generateSubPropertyView() {
  ViewConfig config;
  config.name = ViewNames::INFERRED_PROPERTIES;
  config.required = false;
  config.description =
      "Pre-computed rdfs:subPropertyOf transitive closure for property "
      "inference";
  config.costFactor = 3.0;

  // This view materializes triples using parent properties
  std::ostringstream query;
  query << "SELECT ?subject ?property ?object ?depth WHERE {\n";

  // Direct property usage (depth 0)
  query << "  {\n";
  query << "    ?subject ?property ?object .\n";
  query << "    BIND(0 AS ?depth)\n";
  query << "  }\n";

  // Inferred through subPropertyOf hierarchy
  size_t maxDepth =
      std::min(config_.maxTransitiveDepth, static_cast<size_t>(5));
  for (size_t depth = 1; depth <= maxDepth; ++depth) {
    query << "  UNION\n";
    query << "  {\n";
    query << "    ?subject ?subProp ?object .\n";
    query << "    ?subProp";

    for (size_t i = 0; i < depth; ++i) {
      if (i > 0) {
        query << " ?midProp" << i;
      }
      query << " <http://www.w3.org/2000/01/rdf-schema#subPropertyOf>";
      if (i < depth - 1) {
        query << " ?midProp" << (i + 1) << " .\n    ?midProp" << (i + 1);
      }
    }
    query << " ?property .\n";
    query << "    BIND(" << depth << " AS ?depth)\n";
    query << "  }\n";
  }

  query << "}\n";

  config.query = query.str();
  return config;
}

// ____________________________________________________________________________
ViewConfig OwlViewGenerator::generateInversePropertyView() {
  ViewConfig config;
  config.name = ViewNames::INVERSE_PROPERTIES;
  config.required = false;
  config.description =
      "Pre-computed owl:inverseOf expansions for bidirectional relationships";
  config.costFactor = 1.5;

  // Materialize inverse property relationships
  config.query = R"(
SELECT ?subject ?property ?object WHERE {
  {
    # Direct triples
    ?subject ?property ?object .
  }
  UNION
  {
    # Inverse direction: if ?o ?invProp ?s and ?invProp inverseOf ?prop
    ?object ?invProp ?subject .
    ?invProp <http://www.w3.org/2002/07/owl#inverseOf> ?property .
  }
  UNION
  {
    # Inverse direction: if ?prop inverseOf ?invProp
    ?object ?invProp ?subject .
    ?property <http://www.w3.org/2002/07/owl#inverseOf> ?invProp .
  }
}
)";

  return config;
}

// ____________________________________________________________________________
ViewConfig OwlViewGenerator::generateSameAsView() {
  ViewConfig config;
  config.name = ViewNames::UNIFIED_ENTITIES;
  config.required = false;
  config.description =
      "Pre-computed owl:sameAs unification for entity resolution";
  config.costFactor = 2.5;

  // Materialize canonical entity mappings
  // We choose the lexicographically smallest IRI as canonical
  config.query = R"(
SELECT ?canonical ?alias ?property ?value WHERE {
  {
    # Direct properties
    ?canonical ?property ?value .
    BIND(?canonical AS ?alias)
  }
  UNION
  {
    # Properties from sameAs aliases
    ?alias <http://www.w3.org/2002/07/owl#sameAs> ?canonical .
    ?alias ?property ?value .
    FILTER(STR(?canonical) < STR(?alias))
  }
  UNION
  {
    # Reverse sameAs direction
    ?canonical <http://www.w3.org/2002/07/owl#sameAs> ?alias .
    ?alias ?property ?value .
    FILTER(STR(?canonical) < STR(?alias))
  }
}
)";

  return config;
}

// ____________________________________________________________________________
ViewConfig OwlViewGenerator::generateInterfaceMembershipView() {
  ViewConfig config;
  config.name = ViewNames::INTERFACE_MEMBERSHIP;
  config.required = true;
  config.description =
      "Pre-computed interface (superclass) membership for GraphQL interfaces";
  config.costFactor = 1.5;

  // Find all superclass memberships
  std::ostringstream query;
  query << "SELECT ?instance ?interface ?directClass WHERE {\n";
  query << "  ?instance <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> "
           "?directClass .\n";
  query << "  ?directClass "
           "<http://www.w3.org/2000/01/rdf-schema#subClassOf>+ ?interface .\n";

  // Only include interfaces (classes with multiple subclasses)
  query << "  FILTER EXISTS {\n";
  query << "    ?otherClass "
           "<http://www.w3.org/2000/01/rdf-schema#subClassOf> ?interface .\n";
  query << "    FILTER(?otherClass != ?directClass)\n";
  query << "  }\n";
  query << "  FILTER(!isBlank(?interface))\n";
  query << "}\n";

  config.query = query.str();
  return config;
}

// ____________________________________________________________________________
ViewConfig OwlViewGenerator::generateInstancePropertiesView() {
  ViewConfig config;
  config.name = ViewNames::INSTANCE_PROPERTIES;
  config.required = true;
  config.description =
      "Pre-computed property applicability per instance based on class "
      "hierarchy";
  config.costFactor = 2.0;

  // Materialize which properties apply to each instance through inheritance
  std::ostringstream query;
  query << "SELECT ?instance ?class ?property WHERE {\n";
  query << "  ?instance <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> "
           "?directClass .\n";
  query << "  {\n";
  query << "    # Direct class properties\n";
  query << "    ?property <http://www.w3.org/2000/01/rdf-schema#domain> "
           "?directClass .\n";
  query << "    BIND(?directClass AS ?class)\n";
  query << "  }\n";
  query << "  UNION\n";
  query << "  {\n";
  query << "    # Inherited properties through subClassOf\n";
  query << "    ?directClass "
           "<http://www.w3.org/2000/01/rdf-schema#subClassOf>+ ?class .\n";
  query << "    ?property <http://www.w3.org/2000/01/rdf-schema#domain> "
           "?class .\n";
  query << "  }\n";
  query << "}\n";

  config.query = query.str();
  return config;
}

// ____________________________________________________________________________
ViewConfig OwlViewGenerator::generatePropertyValidationView() {
  ViewConfig config;
  config.name = ViewNames::PROPERTY_VALIDATION;
  config.required = false;
  config.description =
      "Pre-computed domain/range validation for property usage";
  config.costFactor = 3.0;

  // Validate property usage against domain/range
  config.query = R"(
SELECT ?subject ?property ?object ?domainValid ?rangeValid WHERE {
  ?subject ?property ?object .

  # Check domain validity
  OPTIONAL {
    ?property <http://www.w3.org/2000/01/rdf-schema#domain> ?domain .
    ?subject <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> ?subjectType .
    ?subjectType <http://www.w3.org/2000/01/rdf-schema#subClassOf>* ?domain .
    BIND(true AS ?domainMatch)
  }
  BIND(COALESCE(?domainMatch, false) AS ?domainValid)

  # Check range validity (for object properties)
  OPTIONAL {
    ?property <http://www.w3.org/2000/01/rdf-schema#range> ?range .
    ?object <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> ?objectType .
    ?objectType <http://www.w3.org/2000/01/rdf-schema#subClassOf>* ?range .
    BIND(true AS ?rangeMatch)
  }
  BIND(COALESCE(?rangeMatch, true) AS ?rangeValid)
}
)";

  return config;
}

}  // namespace graphql

#endif  // QLEVER_GRAPHQL_SUPPORT
