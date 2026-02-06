// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#include "parser/graphql/GraphQLToSparql.h"

#include <sstream>

#include "parser/GraphPatternOperation.h"
#include "parser/SparqlParser.h"
#include "parser/SparqlTriple.h"
#include "parser/TripleComponent.h"

namespace graphql {

// ============================================================================
// Constructor
// ============================================================================

GraphQLToSparql::GraphQLToSparql(const GraphQLSchema& schema,
                                 TranslationConfig config)
    : schema_(schema), config_(std::move(config)) {}

// ============================================================================
// Variable Generation
// ============================================================================

std::string GraphQLToSparql::nextVar(const std::string& base) {
  return "?" + base + std::to_string(varCounter_++);
}

// ============================================================================
// Main Translation Entry Point
// ============================================================================

std::variant<TranslationResult, std::vector<GraphQLError>>
GraphQLToSparql::translate(
    const Document& document,
    std::optional<std::string_view> operationName,
    const std::unordered_map<std::string, Value>& variables) {
  resetVarCounter();

  // Find the operation to translate
  const Operation* op = document.findOperation(operationName);
  if (!op) {
    GraphQLError err;
    if (operationName.has_value()) {
      err.message =
          "Operation not found: " + std::string(operationName.value());
    } else if (document.operations.size() > 1) {
      err.message =
          "Multiple operations found; operationName is required";
    } else {
      err.message = "No operations found in document";
    }
    return std::vector<GraphQLError>{std::move(err)};
  }

  // Only support queries for now
  if (op->type != OperationType::Query) {
    GraphQLError err;
    err.message = "Only query operations are supported (not mutation/subscription)";
    err.locations.push_back(op->location);
    return std::vector<GraphQLError>{std::move(err)};
  }

  TranslationResult result;
  std::vector<GraphQLError> errors;

  translateOperation(*op, variables, document, result, errors);

  if (!errors.empty()) {
    return errors;
  }

  return result;
}

// ============================================================================
// Operation Translation
// ============================================================================

void GraphQLToSparql::translateOperation(
    const Operation& op,
    const std::unordered_map<std::string, Value>& variables,
    const Document& document,
    TranslationResult& result,
    std::vector<GraphQLError>& errors) {

  // Initialize the ParsedQuery with a SELECT clause
  result.parsedQuery._clause = ParsedQuery::SelectClause{};
  auto& selectClause = std::get<ParsedQuery::SelectClause>(result.parsedQuery._clause);

  // Initialize root graph pattern
  result.parsedQuery._rootGraphPattern = parsedQuery::GraphPattern{};
  result.parsedQuery._rootGraphPattern._optional = false;

  // Set up variables for select
  std::vector<Variable> selectVariables;

  // Initialize root field mapping
  result.rootMapping.graphqlField = "data";
  result.rootMapping.isList = false;

  // Create translation context
  TranslationContext ctx{
      result.parsedQuery,
      result.parsedQuery._rootGraphPattern,
      selectVariables,
      result.rootMapping,
      "",      // No subject yet
      nullptr, // No parent type yet
      errors,
      variables,
      document};

  // Translate each field in the selection set
  translateSelectionSet(op.selectionSet, ctx);

  // Apply pagination from operation arguments
  applyPagination(op.directives.empty() ? std::vector<Argument>{}
                                        : op.directives[0].arguments,
                  result.parsedQuery);

  // Set up SELECT clause with all collected variables
  selectClause.setSelected(std::move(selectVariables));
}

// ============================================================================
// Selection Set Translation
// ============================================================================

void GraphQLToSparql::translateSelectionSet(
    const std::vector<Selection>& selections,
    TranslationContext& ctx) {

  for (const auto& selection : selections) {
    if (auto* fieldPtr = std::get_if<std::shared_ptr<Field>>(&selection)) {
      translateField(**fieldPtr, ctx);
    } else if (auto* spreadPtr = std::get_if<std::shared_ptr<FragmentSpread>>(&selection)) {
      translateFragmentSpread(**spreadPtr, ctx);
    } else if (auto* inlinePtr = std::get_if<std::shared_ptr<InlineFragment>>(&selection)) {
      translateInlineFragment(**inlinePtr, ctx);
    }
  }
}

// ============================================================================
// Field Translation
// ============================================================================

void GraphQLToSparql::translateField(const Field& field,
                                     TranslationContext& ctx) {
  const std::string& fieldName = field.name;

  // Handle introspection fields - these don't translate to SPARQL
  // Introspection (__schema, __type) is handled by Server.cpp using schema metadata
  // __typename is handled by GraphQLResultFormatter using the type name from context
  if (fieldName == "__typename" || fieldName == "__schema" ||
      fieldName == "__type") {
    return;
  }

  // Handle 'id' field specially - it's the subject IRI
  if (fieldName == "id" && !ctx.subjectVar.empty()) {
    // The id is already the subject variable
    ctx.selectVariables.push_back(Variable{ctx.subjectVar});

    TranslationResult::FieldMapping mapping;
    mapping.sparqlVar = ctx.subjectVar;
    mapping.graphqlField = field.responseKey();
    mapping.parentVar = ctx.subjectVar;
    mapping.isList = false;
    mapping.scalarType = ScalarType::ID;
    ctx.fieldMapping.children.push_back(std::move(mapping));
    return;
  }

  // If this is a root field (no subject yet), it represents a type query
  if (ctx.subjectVar.empty()) {
    // This is a type query like `Person { ... }`
    const SchemaType* type = schema_.findType(fieldName);
    if (!type) {
      GraphQLError err;
      err.message = "Unknown type: " + fieldName;
      err.locations.push_back(field.location);
      ctx.errors.push_back(std::move(err));
      return;
    }

    // Create subject variable for this type
    std::string subjectVar = nextVar(fieldName);

    // Add type constraint: ?subject a <TypeIRI>
    SparqlTriple typeTriple = makeTriple(
        subjectVar,
        "a",  // rdf:type
        "<" + type->classIRI + ">");
    addBasicGraphPattern(ctx.currentPattern, typeTriple);

    // Create field mapping for this type
    TranslationResult::FieldMapping typeMapping;
    typeMapping.sparqlVar = subjectVar;
    typeMapping.graphqlField = field.responseKey();
    typeMapping.parentVar = "";
    typeMapping.isList = true;  // Type queries return lists
    typeMapping.objectTypeName = type->graphqlName;

    // Handle filter argument
    const Argument* filterArg = field.findArgument("filter");
    if (filterArg) {
      translateFilter(*filterArg, subjectVar, *type, ctx);
    }

    // Handle 'id' argument for single-item lookup
    const Argument* idArg = field.findArgument("id");
    if (idArg) {
      Value resolved = resolveValue(idArg->value, ctx.variables);
      if (!resolved.isNull()) {
        std::string filterExpr = subjectVar + " = <" + resolved.asString() + ">";
        addFilter(ctx.currentPattern, filterExpr);
        typeMapping.isList = false;  // Single item lookup
      }
    }

    // Handle pagination
    const Argument* firstArg = field.findArgument("first");
    const Argument* offsetArg = field.findArgument("offset");
    if (firstArg || offsetArg) {
      applyPagination(field.arguments, ctx.query);
    }

    // Handle ordering
    const Argument* orderByArg = field.findArgument("orderBy");
    if (orderByArg) {
      applyOrdering(field.arguments, subjectVar, *type, ctx.query);
    }

    // Create new context for nested fields
    TranslationContext nestedCtx{
        ctx.query,
        ctx.currentPattern,
        ctx.selectVariables,
        typeMapping,
        subjectVar,
        type,
        ctx.errors,
        ctx.variables,
        ctx.document};

    // Translate nested selection set
    if (!field.selectionSet.empty()) {
      translateSelectionSet(field.selectionSet, nestedCtx);
    }

    ctx.fieldMapping.children.push_back(std::move(typeMapping));
    return;
  }

  // This is a field on an existing type
  if (!ctx.parentType) {
    GraphQLError err;
    err.message = "Field '" + fieldName + "' has no parent type context";
    err.locations.push_back(field.location);
    ctx.errors.push_back(std::move(err));
    return;
  }

  // Find the field in the schema
  const SchemaField* schemaField = ctx.parentType->findField(fieldName);
  if (!schemaField) {
    GraphQLError err;
    err.message = "Unknown field '" + fieldName + "' on type '" +
                  ctx.parentType->graphqlName + "'";
    err.locations.push_back(field.location);
    ctx.errors.push_back(std::move(err));
    return;
  }

  // Create variable for this field
  std::string fieldVar = nextVar(fieldName);

  // Create field mapping
  TranslationResult::FieldMapping fieldMapping;
  fieldMapping.sparqlVar = fieldVar;
  fieldMapping.graphqlField = field.responseKey();
  fieldMapping.parentVar = ctx.subjectVar;
  fieldMapping.isList = schemaField->isList;
  fieldMapping.scalarType = schemaField->scalarType;
  fieldMapping.objectTypeName = schemaField->objectTypeName;

  // Add triple pattern for this field
  SparqlTriple fieldTriple = makeTriple(
      ctx.subjectVar,
      "<" + schemaField->propertyIRI + ">",
      fieldVar);

  // Make it OPTIONAL unless explicitly required
  if (schemaField->isRequired) {
    addBasicGraphPattern(ctx.currentPattern, fieldTriple);
  } else {
    addOptionalPattern(ctx.currentPattern, {fieldTriple});
  }

  // Add to select variables
  ctx.selectVariables.push_back(Variable{fieldVar});

  // If this is an object property with nested selections, recurse
  if (schemaField->isObjectProperty && !field.selectionSet.empty()) {
    const SchemaType* nestedType = schema_.findType(schemaField->objectTypeName);
    if (nestedType) {
      TranslationContext nestedCtx{
          ctx.query,
          ctx.currentPattern,
          ctx.selectVariables,
          fieldMapping,
          fieldVar,
          nestedType,
          ctx.errors,
          ctx.variables,
          ctx.document};

      translateSelectionSet(field.selectionSet, nestedCtx);
    }
  }

  ctx.fieldMapping.children.push_back(std::move(fieldMapping));
}

// ============================================================================
// Fragment Translation
// ============================================================================

void GraphQLToSparql::translateFragmentSpread(const FragmentSpread& spread,
                                               TranslationContext& ctx) {
  // Security: Check for fragment cycles
  if (ctx.expandedFragments.count(spread.name) > 0) {
    GraphQLError err;
    err.message = "Fragment cycle detected: " + spread.name;
    err.locations.push_back(spread.location);
    ctx.errors.push_back(std::move(err));
    return;
  }

  const FragmentDefinition* frag = ctx.document.findFragment(spread.name);
  if (!frag) {
    GraphQLError err;
    err.message = "Unknown fragment: " + spread.name;
    err.locations.push_back(spread.location);
    ctx.errors.push_back(std::move(err));
    return;
  }

  // Security: Track this fragment to detect cycles
  ctx.expandedFragments.insert(spread.name);

  // Check type condition compatibility
  // The fragment's type condition must match or be a supertype of the current context type
  if (ctx.parentType && !frag->typeCondition.empty()) {
    const std::string& fragTypeName = frag->typeCondition;
    const std::string& contextTypeName = ctx.parentType->graphqlName;

    // For now, require exact type match
    // A full implementation would check type hierarchy (interfaces, unions)
    if (fragTypeName != contextTypeName) {
      // Check if the fragment type exists in schema
      const SchemaType* fragType = schema_.findType(fragTypeName);
      if (!fragType) {
        GraphQLError err;
        err.message = "Fragment '" + spread.name + "' has unknown type condition: " +
                      fragTypeName;
        err.locations.push_back(spread.location);
        ctx.errors.push_back(std::move(err));
        ctx.expandedFragments.erase(spread.name);
        return;
      }

      // Type mismatch - warn but continue (GraphQL allows this for interfaces/unions)
      // In strict mode, this could be an error
      AD_LOG_DEBUG << "Fragment '" << spread.name << "' type condition '"
                   << fragTypeName << "' differs from context type '"
                   << contextTypeName << "'" << std::endl;
    }
  }

  translateSelectionSet(frag->selectionSet, ctx);

  // Remove from tracking after expansion
  ctx.expandedFragments.erase(spread.name);
}

void GraphQLToSparql::translateInlineFragment(const InlineFragment& fragment,
                                               TranslationContext& ctx) {
  // Handle type condition for inline fragments
  // An inline fragment like "... on Person { name }" should only include
  // fields when the entity is of type Person
  if (fragment.typeCondition.has_value() && !fragment.typeCondition->empty()) {
    const std::string& typeName = *fragment.typeCondition;
    const SchemaType* fragType = schema_.findType(typeName);

    if (!fragType) {
      GraphQLError err;
      err.message = "Inline fragment has unknown type condition: " + typeName;
      err.locations.push_back(fragment.location);
      ctx.errors.push_back(std::move(err));
      return;
    }

    // For inline fragments with type conditions, we create an OPTIONAL block
    // with a type constraint. This ensures fields are only included when
    // the entity matches the type condition.
    if (ctx.parentType && typeName != ctx.parentType->graphqlName) {
      // Different type - wrap in OPTIONAL with type check
      // This handles polymorphic queries where an entity might be multiple types
      parsedQuery::GraphPattern optionalPattern;
      optionalPattern._optional = true;

      // Add type constraint triple: ?subject a <TypeIRI>
      if (!ctx.subjectVar.empty() && !fragType->classIRI.empty()) {
        SparqlTriple typeTriple = makeTriple(
            ctx.subjectVar,
            "a",
            "<" + fragType->classIRI + ">");
        parsedQuery::BasicGraphPattern bgp;
        bgp._triples.push_back(typeTriple);
        optionalPattern._graphPatterns.push_back(std::move(bgp));
      }

      // Create a new context for the inline fragment with the fragment's type
      TranslationContext fragCtx{
          ctx.query,
          optionalPattern,
          ctx.selectVariables,
          ctx.fieldMapping,
          ctx.subjectVar,
          fragType,  // Use the fragment's type as parent
          ctx.errors,
          ctx.variables,
          ctx.document,
          ctx.expandedFragments};

      translateSelectionSet(fragment.selectionSet, fragCtx);

      ctx.currentPattern._graphPatterns.push_back(
          parsedQuery::Optional{std::move(optionalPattern)});
      return;
    }
  }

  // No type condition or same type - just translate the selection set
  translateSelectionSet(fragment.selectionSet, ctx);
}

// ============================================================================
// Filter Translation
// ============================================================================

void GraphQLToSparql::translateFilter(const Argument& filterArg,
                                       const std::string& subjectVar,
                                       const SchemaType& type,
                                       TranslationContext& ctx) {
  Value resolved = resolveValue(filterArg.value, ctx.variables);
  if (resolved.type != Value::Type::Object) {
    return;
  }

  const auto& fields =
      std::get<std::vector<std::pair<std::string, Value>>>(resolved.data);

  for (const auto& [fieldName, filterValue] : fields) {
    // Handle logical operators
    if (fieldName == "AND") {
      if (filterValue.type == Value::Type::List) {
        const auto& andFilters = std::get<std::vector<Value>>(filterValue.data);
        for (const auto& subFilter : andFilters) {
          Argument subArg{"filter", subFilter, filterArg.location};
          translateFilter(subArg, subjectVar, type, ctx);
        }
      }
    } else if (fieldName == "OR" || fieldName == "_or") {
      translateOrFilter(filterValue, subjectVar, type, ctx);
    } else if (fieldName == "NOT" || fieldName == "_not") {
      translateNotFilter(filterValue, subjectVar, type, ctx);
    } else if (fieldName == "id") {
      translateIdFilter(subjectVar, filterValue, ctx);
    } else {
      translateFilterField(fieldName, filterValue, subjectVar, type, ctx);
    }
  }
}

void GraphQLToSparql::translateFilterField(const std::string& fieldName,
                                            const Value& filterValue,
                                            const std::string& subjectVar,
                                            const SchemaType& type,
                                            TranslationContext& ctx) {
  const SchemaField* field = type.findField(fieldName);
  if (!field) {
    return;  // Unknown field in filter, ignore
  }

  // Create a variable for filtering
  std::string filterVar = nextVar("filter_" + fieldName);

  // Add triple pattern for the field
  SparqlTriple filterTriple = makeTriple(
      subjectVar,
      "<" + field->propertyIRI + ">",
      filterVar);
  addBasicGraphPattern(ctx.currentPattern, filterTriple);

  // Apply the filter based on type
  if (field->isObjectProperty) {
    translateIdFilter(filterVar, filterValue, ctx);
  } else {
    switch (field->scalarType) {
      case ScalarType::STRING:
        translateStringFilter(filterVar, filterValue, ctx);
        break;
      case ScalarType::INT:
        translateNumericFilter(filterVar, filterValue, false, ctx);
        break;
      case ScalarType::FLOAT:
        translateNumericFilter(filterVar, filterValue, true, ctx);
        break;
      case ScalarType::BOOLEAN:
        if (filterValue.type == Value::Type::Object) {
          const auto& fields =
              std::get<std::vector<std::pair<std::string, Value>>>(filterValue.data);
          for (const auto& [op, val] : fields) {
            if (op == "eq" && val.type == Value::Type::Boolean) {
              std::string boolStr = val.asBool() ? "true" : "false";
              addFilter(ctx.currentPattern,
                        "STR(" + filterVar + ") = \"" + boolStr + "\"");
            }
          }
        }
        break;
      default:
        translateStringFilter(filterVar, filterValue, ctx);
    }
  }
}

void GraphQLToSparql::translateStringFilter(const std::string& varName,
                                             const Value& filterValue,
                                             TranslationContext& ctx) {
  if (filterValue.type != Value::Type::Object) {
    return;
  }

  const auto& fields =
      std::get<std::vector<std::pair<std::string, Value>>>(filterValue.data);

  for (const auto& [op, val] : fields) {
    Value resolved = resolveValue(val, ctx.variables);
    if (resolved.isNull()) continue;

    std::string strVal = resolved.asString();
    // Escape quotes in string
    size_t pos = 0;
    while ((pos = strVal.find('"', pos)) != std::string::npos) {
      strVal.replace(pos, 1, "\\\"");
      pos += 2;
    }

    if (op == "eq") {
      addFilter(ctx.currentPattern,
                "STR(" + varName + ") = \"" + strVal + "\"");
    } else if (op == "ne") {
      addFilter(ctx.currentPattern,
                "STR(" + varName + ") != \"" + strVal + "\"");
    } else if (op == "contains") {
      addFilter(ctx.currentPattern,
                "CONTAINS(STR(" + varName + "), \"" + strVal + "\")");
    } else if (op == "containsIgnoreCase") {
      addFilter(ctx.currentPattern,
                "CONTAINS(LCASE(STR(" + varName + ")), LCASE(\"" + strVal + "\"))");
    } else if (op == "startsWith") {
      addFilter(ctx.currentPattern,
                "STRSTARTS(STR(" + varName + "), \"" + strVal + "\")");
    } else if (op == "endsWith") {
      addFilter(ctx.currentPattern,
                "STRENDS(STR(" + varName + "), \"" + strVal + "\")");
    } else if (op == "regex") {
      addFilter(ctx.currentPattern,
                "REGEX(STR(" + varName + "), \"" + strVal + "\")");
    } else if (op == "lang") {
      addFilter(ctx.currentPattern,
                "LANG(" + varName + ") = \"" + strVal + "\"");
    }
  }
}

void GraphQLToSparql::translateNumericFilter(const std::string& varName,
                                              const Value& filterValue,
                                              bool isFloat,
                                              TranslationContext& ctx) {
  if (filterValue.type != Value::Type::Object) {
    return;
  }

  const auto& fields =
      std::get<std::vector<std::pair<std::string, Value>>>(filterValue.data);

  for (const auto& [op, val] : fields) {
    Value resolved = resolveValue(val, ctx.variables);
    if (resolved.isNull()) continue;

    std::string numStr;
    if (isFloat) {
      numStr = std::to_string(resolved.asFloat());
    } else {
      numStr = std::to_string(resolved.asInt());
    }

    if (op == "eq") {
      addFilter(ctx.currentPattern, varName + " = " + numStr);
    } else if (op == "ne") {
      addFilter(ctx.currentPattern, varName + " != " + numStr);
    } else if (op == "gt") {
      addFilter(ctx.currentPattern, varName + " > " + numStr);
    } else if (op == "gte") {
      addFilter(ctx.currentPattern, varName + " >= " + numStr);
    } else if (op == "lt") {
      addFilter(ctx.currentPattern, varName + " < " + numStr);
    } else if (op == "lte") {
      addFilter(ctx.currentPattern, varName + " <= " + numStr);
    } else if (op == "between" && val.type == Value::Type::List) {
      const auto& range = std::get<std::vector<Value>>(val.data);
      if (range.size() == 2) {
        std::string minStr = isFloat ? std::to_string(range[0].asFloat())
                                     : std::to_string(range[0].asInt());
        std::string maxStr = isFloat ? std::to_string(range[1].asFloat())
                                     : std::to_string(range[1].asInt());
        addFilter(ctx.currentPattern,
                  varName + " >= " + minStr + " && " + varName + " <= " + maxStr);
      }
    }
  }
}

void GraphQLToSparql::translateIdFilter(const std::string& subjectVar,
                                         const Value& filterValue,
                                         TranslationContext& ctx) {
  if (filterValue.type != Value::Type::Object) {
    return;
  }

  const auto& fields =
      std::get<std::vector<std::pair<std::string, Value>>>(filterValue.data);

  for (const auto& [op, val] : fields) {
    Value resolved = resolveValue(val, ctx.variables);
    if (resolved.isNull()) continue;

    if (op == "eq") {
      addFilter(ctx.currentPattern,
                subjectVar + " = <" + resolved.asString() + ">");
    } else if (op == "ne") {
      addFilter(ctx.currentPattern,
                subjectVar + " != <" + resolved.asString() + ">");
    } else if (op == "in" && val.type == Value::Type::List) {
      const auto& values = std::get<std::vector<Value>>(val.data);
      if (!values.empty()) {
        std::ostringstream oss;
        oss << subjectVar << " IN (";
        bool first = true;
        for (const auto& v : values) {
          if (!first) oss << ", ";
          oss << "<" << v.asString() << ">";
          first = false;
        }
        oss << ")";
        addFilter(ctx.currentPattern, oss.str());
      }
    }
  }
}

// ============================================================================
// Pagination and Ordering
// ============================================================================

void GraphQLToSparql::applyPagination(const std::vector<Argument>& arguments,
                                       ParsedQuery& query) {
  for (const auto& arg : arguments) {
    if (arg.name == "first" || arg.name == "limit") {
      if (arg.value.type == Value::Type::Int) {
        int64_t requestedLimit = arg.value.asInt();
        // Security: Enforce maximum limit and reject negative values
        if (requestedLimit < 0) {
          requestedLimit = 0;
        } else if (static_cast<uint64_t>(requestedLimit) > config_.maxResults) {
          requestedLimit = static_cast<int64_t>(config_.maxResults);
        }
        query._limitOffset._limit = static_cast<uint64_t>(requestedLimit);
      }
    } else if (arg.name == "offset" || arg.name == "skip") {
      if (arg.value.type == Value::Type::Int) {
        int64_t requestedOffset = arg.value.asInt();
        // Security: Enforce maximum offset and reject negative values
        if (requestedOffset < 0) {
          requestedOffset = 0;
        } else if (static_cast<uint64_t>(requestedOffset) > config_.maxOffset) {
          requestedOffset = static_cast<int64_t>(config_.maxOffset);
        }
        query._limitOffset._offset = static_cast<uint64_t>(requestedOffset);
      }
    }
  }
}

void GraphQLToSparql::applyOrdering(const std::vector<Argument>& arguments,
                                     const std::string& subjectVar,
                                     const SchemaType& type,
                                     ParsedQuery& query) {
  std::string orderField;
  bool descending = false;

  for (const auto& arg : arguments) {
    if (arg.name == "orderBy" && arg.value.type == Value::Type::String) {
      orderField = arg.value.asString();
    } else if (arg.name == "orderDirection" &&
               arg.value.type == Value::Type::Enum) {
      descending = (arg.value.asString() == "DESC");
    }
  }

  if (!orderField.empty()) {
    // Find the field in schema
    const SchemaField* field = type.findField(orderField);
    if (field) {
      // The variable name should match what we created during translation
      // For now, use a simple naming scheme
      Variable orderVar{"?" + orderField + "0"};
      query._orderBy.push_back(
          VariableOrderKey{orderVar, descending});
    }
  }
}

// ============================================================================
// Helper Methods
// ============================================================================

Value GraphQLToSparql::resolveValue(
    const Value& value,
    const std::unordered_map<std::string, Value>& variables) {
  // If it's a variable reference, look it up
  if (value.type == Value::Type::String) {
    const std::string& str = std::get<std::string>(value.data);
    if (!str.empty() && str[0] == '$') {
      std::string varName = str.substr(1);
      auto it = variables.find(varName);
      if (it != variables.end()) {
        return it->second;
      }
      return Value::makeNull();
    }
  }
  return value;
}

std::string GraphQLToSparql::valueToSparqlLiteral(const Value& value) {
  switch (value.type) {
    case Value::Type::Null:
      return "UNDEF";
    case Value::Type::Int:
      return std::to_string(std::get<int64_t>(value.data));
    case Value::Type::Float:
      return std::to_string(std::get<double>(value.data));
    case Value::Type::Boolean:
      return std::get<bool>(value.data) ? "true" : "false";
    case Value::Type::String:
    case Value::Type::Enum:
      return "\"" + std::get<std::string>(value.data) + "\"";
    default:
      return "UNDEF";
  }
}

SparqlTriple GraphQLToSparql::makeTriple(const std::string& subject,
                                          const std::string& predicate,
                                          const std::string& object) {
  // Parse subject
  TripleComponent subjectComponent =
      subject.starts_with("?")
          ? TripleComponent{Variable{subject}}
          : TripleComponent{TripleComponent::Iri::fromIriref(subject)};

  // Parse object
  TripleComponent objectComponent =
      object.starts_with("?")
          ? TripleComponent{Variable{object}}
          : TripleComponent{TripleComponent::Iri::fromIriref(object)};

  // Parse predicate - SparqlTriple requires VarOrPath (Variable or PropertyPath)
  // Use the SparqlTriple constructor that takes an IRI directly
  if (predicate == "a") {
    return SparqlTriple{
        std::move(subjectComponent),
        TripleComponent::Iri::fromIriref("<http://www.w3.org/1999/02/22-rdf-syntax-ns#type>"),
        std::move(objectComponent)};
  } else if (predicate.starts_with("?")) {
    return SparqlTriple{
        std::move(subjectComponent),
        Variable{predicate},
        std::move(objectComponent)};
  } else {
    return SparqlTriple{
        std::move(subjectComponent),
        TripleComponent::Iri::fromIriref(predicate),
        std::move(objectComponent)};
  }
}

void GraphQLToSparql::addBasicGraphPattern(parsedQuery::GraphPattern& pattern,
                                            const SparqlTriple& triple) {
  // Find or create a BasicGraphPattern in the graph pattern
  if (pattern._graphPatterns.empty() ||
      !std::holds_alternative<parsedQuery::BasicGraphPattern>(
          pattern._graphPatterns.back())) {
    pattern._graphPatterns.push_back(parsedQuery::BasicGraphPattern{});
  }

  auto& bgp = std::get<parsedQuery::BasicGraphPattern>(
      pattern._graphPatterns.back());
  bgp._triples.push_back(triple);
}

void GraphQLToSparql::addOptionalPattern(
    parsedQuery::GraphPattern& pattern,
    const std::vector<SparqlTriple>& triples) {
  parsedQuery::GraphPattern optionalPattern;
  optionalPattern._optional = true;

  parsedQuery::BasicGraphPattern bgp;
  bgp._triples = triples;
  optionalPattern._graphPatterns.push_back(std::move(bgp));

  pattern._graphPatterns.push_back(
      parsedQuery::Optional{std::move(optionalPattern)});
}

void GraphQLToSparql::addFilter(parsedQuery::GraphPattern& pattern,
                                 const std::string& expression) {
  // Parse the filter expression by wrapping it in a minimal SPARQL query
  // and extracting the filter from the parsed result.
  // This leverages QLever's full SPARQL expression parser.
  try {
    // Create a minimal query with the filter
    // We use a dummy variable and triple to satisfy SPARQL syntax
    std::string wrappedQuery =
        "SELECT * WHERE { ?_dummy ?_p ?_o . FILTER(" + expression + ") }";

    // Parse the query - note: we pass nullptr for encodedIriManager since
    // we don't need IRI encoding for filter expressions
    auto parsedQuery = SparqlParser::parseQuery(nullptr, wrappedQuery, {});

    // Extract filters from the parsed query's graph pattern
    const auto& rootPattern = parsedQuery._rootGraphPattern;
    if (!rootPattern._filters.empty()) {
      // Add the parsed filter(s) to our pattern
      for (const auto& filter : rootPattern._filters) {
        pattern._filters.push_back(filter);
      }
    }
  } catch (const std::exception& e) {
    // If parsing fails, log a warning but don't fail the query
    // This can happen with malformed filter expressions
    AD_LOG_WARN << "Failed to parse filter expression '" << expression
                << "': " << e.what() << std::endl;
  }
}

// ============================================================================
// OR Filter (UNION) Translation
// ============================================================================

void GraphQLToSparql::translateOrFilter(const Value& orFilters,
                                         const std::string& subjectVar,
                                         const SchemaType& type,
                                         TranslationContext& ctx) {
  // OR filter expects a list of filter objects
  // Each filter in the list should match one branch of the UNION
  if (orFilters.type != Value::Type::List) {
    return;
  }

  const auto& filters = std::get<std::vector<Value>>(orFilters.data);
  if (filters.empty()) {
    return;
  }

  // For a single filter, treat it as a regular AND
  if (filters.size() == 1) {
    Argument subArg{"filter", filters[0], {}};
    translateFilter(subArg, subjectVar, type, ctx);
    return;
  }

  // For multiple filters, we need to create a UNION
  // UNION in SPARQL: { pattern1 } UNION { pattern2 } UNION { pattern3 }
  // In QLever's representation, Union is binary, so we need to nest them:
  // Union(pattern1, Union(pattern2, pattern3))

  // Create graph patterns for each OR branch
  std::vector<parsedQuery::GraphPattern> branches;
  for (const auto& filterObj : filters) {
    parsedQuery::GraphPattern branchPattern;
    branchPattern._optional = false;

    // Create a temporary context for this branch
    TranslationContext branchCtx{
        ctx.query,
        branchPattern,
        ctx.selectVariables,
        ctx.fieldMapping,
        subjectVar,
        ctx.parentType,
        ctx.errors,
        ctx.variables,
        ctx.document};

    // Translate the filter for this branch
    Argument subArg{"filter", filterObj, {}};
    translateFilter(subArg, subjectVar, type, branchCtx);

    branches.push_back(std::move(branchPattern));
  }

  // Build the UNION from right to left (nesting binary unions)
  if (branches.size() >= 2) {
    // Start with the last two branches
    parsedQuery::Union currentUnion{std::move(branches[branches.size() - 2]),
                                    std::move(branches[branches.size() - 1])};

    // Add remaining branches from right to left
    for (size_t i = branches.size() - 2; i > 0; --i) {
      parsedQuery::GraphPattern unionWrapper;
      unionWrapper._optional = false;
      unionWrapper._graphPatterns.push_back(std::move(currentUnion));

      currentUnion = parsedQuery::Union{std::move(branches[i - 1]),
                                        std::move(unionWrapper)};
    }

    // Add the final UNION to the current pattern
    ctx.currentPattern._graphPatterns.push_back(std::move(currentUnion));
  }
}

// ============================================================================
// NOT Filter (MINUS) Translation
// ============================================================================

void GraphQLToSparql::translateNotFilter(const Value& notFilter,
                                          const std::string& subjectVar,
                                          const SchemaType& type,
                                          TranslationContext& ctx) {
  // NOT filter expects a filter object
  // Translates to SPARQL MINUS { ... } pattern
  if (notFilter.type != Value::Type::Object) {
    return;
  }

  // Create a graph pattern for the MINUS branch
  parsedQuery::GraphPattern minusPattern;
  minusPattern._optional = false;

  // Create a temporary context for the MINUS branch
  TranslationContext minusCtx{
      ctx.query,
      minusPattern,
      ctx.selectVariables,
      ctx.fieldMapping,
      subjectVar,
      ctx.parentType,
      ctx.errors,
      ctx.variables,
      ctx.document};

  // Translate the filter for the MINUS branch
  Argument subArg{"filter", notFilter, {}};
  translateFilter(subArg, subjectVar, type, minusCtx);

  // Only add MINUS if there are patterns to negate
  if (!minusPattern._graphPatterns.empty()) {
    ctx.currentPattern._graphPatterns.push_back(
        parsedQuery::Minus{std::move(minusPattern)});
  }
}

}  // namespace graphql
