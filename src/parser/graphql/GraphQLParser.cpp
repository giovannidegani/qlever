// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#include "parser/graphql/GraphQLParser.h"

#include <functional>
#include <stdexcept>

namespace graphql {

// ============================================================================
// GraphQLError
// ============================================================================

nlohmann::json GraphQLError::toJSON() const {
  nlohmann::json result;
  result["message"] = message;

  if (!locations.empty()) {
    nlohmann::json locs = nlohmann::json::array();
    for (const auto& loc : locations) {
      locs.push_back({{"line", loc.line}, {"column", loc.column}});
    }
    result["locations"] = std::move(locs);
  }

  if (!path.empty()) {
    nlohmann::json pathArray = nlohmann::json::array();
    for (const auto& segment : path) {
      if (std::holds_alternative<std::string>(segment)) {
        pathArray.push_back(std::get<std::string>(segment));
      } else {
        pathArray.push_back(std::get<size_t>(segment));
      }
    }
    result["path"] = std::move(pathArray);
  }

  if (!extensions.empty()) {
    nlohmann::json extJson;
    for (const auto& [key, value] : extensions) {
      extJson[key] = value;
    }
    result["extensions"] = std::move(extJson);
  }

  return result;
}

// ============================================================================
// Value
// ============================================================================

Value Value::makeNull() { return Value{Type::Null, std::monostate{}}; }

Value Value::makeInt(int64_t val) { return Value{Type::Int, val}; }

Value Value::makeFloat(double val) { return Value{Type::Float, val}; }

Value Value::makeString(std::string val) {
  return Value{Type::String, std::move(val)};
}

Value Value::makeBool(bool val) { return Value{Type::Boolean, val}; }

Value Value::makeEnum(std::string val) {
  return Value{Type::Enum, std::move(val)};
}

Value Value::makeList(std::vector<Value> vals) {
  return Value{Type::List, std::move(vals)};
}

Value Value::makeObject(std::vector<std::pair<std::string, Value>> fields) {
  return Value{Type::Object, std::move(fields)};
}

const std::string& Value::asString() const {
  if (type != Type::String && type != Type::Enum) {
    throw std::runtime_error("Value is not a string");
  }
  return std::get<std::string>(data);
}

int64_t Value::asInt() const {
  if (type != Type::Int) {
    throw std::runtime_error("Value is not an int");
  }
  return std::get<int64_t>(data);
}

double Value::asFloat() const {
  if (type != Type::Float) {
    throw std::runtime_error("Value is not a float");
  }
  return std::get<double>(data);
}

bool Value::asBool() const {
  if (type != Type::Boolean) {
    throw std::runtime_error("Value is not a boolean");
  }
  return std::get<bool>(data);
}

const std::vector<Value>& Value::asList() const {
  if (type != Type::List) {
    throw std::runtime_error("Value is not a list");
  }
  return std::get<std::vector<Value>>(data);
}

const std::vector<std::pair<std::string, Value>>& Value::asObject() const {
  if (type != Type::Object) {
    throw std::runtime_error("Value is not an object");
  }
  return std::get<std::vector<std::pair<std::string, Value>>>(data);
}

// ============================================================================
// Field
// ============================================================================

const Argument* Field::findArgument(std::string_view argName) const {
  for (const auto& arg : arguments) {
    if (arg.name == argName) {
      return &arg;
    }
  }
  return nullptr;
}

// ============================================================================
// Document
// ============================================================================

const Operation* Document::findOperation(
    std::optional<std::string_view> operationName) const {
  if (operations.empty()) {
    return nullptr;
  }

  if (!operationName.has_value()) {
    // If no name specified and only one operation, return it
    if (operations.size() == 1) {
      return &operations[0];
    }
    // Multiple operations require a name
    return nullptr;
  }

  for (const auto& op : operations) {
    if (op.name.has_value() && op.name.value() == operationName.value()) {
      return &op;
    }
  }
  return nullptr;
}

const FragmentDefinition* Document::findFragment(std::string_view name) const {
  for (const auto& frag : fragments) {
    if (frag.name == name) {
      return &frag;
    }
  }
  return nullptr;
}

// ============================================================================
// GraphQL Parser Implementation
// ============================================================================

// Helper: Skip whitespace in a string
static size_t skipWhitespace(const std::string& str, size_t pos) {
  while (pos < str.size() && std::isspace(str[pos])) pos++;
  return pos;
}

// Helper: Calculate line and column from position in string
static SourceLocation getLocationFromPos(const std::string& str, size_t pos) {
  SourceLocation loc;
  loc.line = 1;
  loc.column = 1;
  for (size_t i = 0; i < pos && i < str.size(); ++i) {
    if (str[i] == '\n') {
      loc.line++;
      loc.column = 1;
    } else {
      loc.column++;
    }
  }
  return loc;
}

// Helper: Read an identifier (alphanumeric + underscore)
static std::string readIdentifier(const std::string& str, size_t& pos) {
  size_t start = pos;
  while (pos < str.size() &&
         (std::isalnum(str[pos]) || str[pos] == '_')) {
    pos++;
  }
  return str.substr(start, pos - start);
}

// Helper: Find matching closing brace, accounting for nesting
static size_t findMatchingBrace(const std::string& str, size_t openPos) {
  if (openPos >= str.size() || str[openPos] != '{') {
    return std::string::npos;
  }
  size_t depth = 1;
  size_t pos = openPos + 1;
  while (pos < str.size() && depth > 0) {
    if (str[pos] == '{') depth++;
    else if (str[pos] == '}') depth--;
    pos++;
  }
  return depth == 0 ? pos - 1 : std::string::npos;
}

// Forward declarations for mutually recursive functions
static Value parseValue(const std::string& str, size_t& pos,
                        std::vector<GraphQLError>& errors);

// Helper: Parse a GraphQL Value (string, int, float, boolean, null, list, object, enum)
static Value parseValue(const std::string& str, size_t& pos,
                        std::vector<GraphQLError>& errors) {
  pos = skipWhitespace(str, pos);
  if (pos >= str.size()) {
    errors.push_back({.message = "Unexpected end of input while parsing value"});
    return Value::makeNull();
  }

  char c = str[pos];

  // String literal
  if (c == '"') {
    pos++;  // skip opening quote
    size_t start = pos;
    std::string value;
    while (pos < str.size() && str[pos] != '"') {
      if (str[pos] == '\\' && pos + 1 < str.size()) {
        pos++;  // skip backslash
        // Handle escape sequences
        switch (str[pos]) {
          case 'n': value += '\n'; break;
          case 'r': value += '\r'; break;
          case 't': value += '\t'; break;
          case '\\': value += '\\'; break;
          case '"': value += '"'; break;
          default: value += str[pos]; break;
        }
        pos++;
      } else {
        value += str[pos++];
      }
    }
    if (pos < str.size()) pos++;  // skip closing quote
    return Value::makeString(std::move(value));
  }

  // List
  if (c == '[') {
    pos++;  // skip opening bracket
    std::vector<Value> listItems;

    pos = skipWhitespace(str, pos);
    while (pos < str.size() && str[pos] != ']') {
      listItems.push_back(parseValue(str, pos, errors));
      pos = skipWhitespace(str, pos);
      if (pos < str.size() && str[pos] == ',') {
        pos++;
        pos = skipWhitespace(str, pos);
      }
    }
    if (pos < str.size()) pos++;  // skip closing bracket
    return Value::makeList(std::move(listItems));
  }

  // Object
  if (c == '{') {
    pos++;  // skip opening brace
    std::vector<std::pair<std::string, Value>> fields;

    pos = skipWhitespace(str, pos);
    while (pos < str.size() && str[pos] != '}') {
      // Parse field name
      std::string fieldName = readIdentifier(str, pos);
      pos = skipWhitespace(str, pos);

      // Expect colon
      if (pos < str.size() && str[pos] == ':') {
        pos++;
        pos = skipWhitespace(str, pos);
      }

      // Parse field value
      Value fieldValue = parseValue(str, pos, errors);
      fields.emplace_back(std::move(fieldName), std::move(fieldValue));

      pos = skipWhitespace(str, pos);
      if (pos < str.size() && str[pos] == ',') {
        pos++;
        pos = skipWhitespace(str, pos);
      }
    }
    if (pos < str.size()) pos++;  // skip closing brace
    return Value::makeObject(std::move(fields));
  }

  // Variable reference ($name) - store as enum with $ prefix to distinguish
  if (c == '$') {
    pos++;  // skip $
    std::string varName = "$" + readIdentifier(str, pos);
    return Value::makeEnum(std::move(varName));
  }

  // Number (int or float)
  if (c == '-' || std::isdigit(c)) {
    size_t start = pos;
    if (str[pos] == '-') pos++;
    while (pos < str.size() && std::isdigit(str[pos])) pos++;

    bool isFloat = false;
    if (pos < str.size() && str[pos] == '.') {
      isFloat = true;
      pos++;
      while (pos < str.size() && std::isdigit(str[pos])) pos++;
    }
    if (pos < str.size() && (str[pos] == 'e' || str[pos] == 'E')) {
      isFloat = true;
      pos++;
      if (pos < str.size() && (str[pos] == '+' || str[pos] == '-')) pos++;
      while (pos < str.size() && std::isdigit(str[pos])) pos++;
    }

    std::string numStr = str.substr(start, pos - start);
    if (isFloat) {
      return Value::makeFloat(std::stod(numStr));
    } else {
      return Value::makeInt(std::stoll(numStr));
    }
  }

  // Boolean or null or enum
  std::string ident = readIdentifier(str, pos);
  if (ident == "true") {
    return Value::makeBool(true);
  }
  if (ident == "false") {
    return Value::makeBool(false);
  }
  if (ident == "null") {
    return Value::makeNull();
  }

  // Enum value
  return Value::makeEnum(std::move(ident));
}

// Helper: Parse arguments from (name: value, ...) format
static void parseArguments(const std::string& str, size_t& pos,
                           std::vector<Argument>& arguments,
                           std::vector<GraphQLError>& errors) {
  if (pos >= str.size() || str[pos] != '(') {
    return;
  }

  pos++;  // skip opening paren
  pos = skipWhitespace(str, pos);

  while (pos < str.size() && str[pos] != ')') {
    Argument arg;
    arg.location = getLocationFromPos(str, pos);

    // Parse argument name
    arg.name = readIdentifier(str, pos);
    if (arg.name.empty()) {
      errors.push_back({.message = "Expected argument name"});
      break;
    }

    pos = skipWhitespace(str, pos);

    // Expect colon
    if (pos >= str.size() || str[pos] != ':') {
      errors.push_back({.message = "Expected ':' after argument name"});
      break;
    }
    pos++;  // skip colon
    pos = skipWhitespace(str, pos);

    // Parse value
    arg.value = parseValue(str, pos, errors);

    arguments.push_back(std::move(arg));

    pos = skipWhitespace(str, pos);

    // Check for comma
    if (pos < str.size() && str[pos] == ',') {
      pos++;
      pos = skipWhitespace(str, pos);
    }
  }

  if (pos < str.size() && str[pos] == ')') {
    pos++;  // skip closing paren
  }
}

// Note: Security limits are defined in headers and configurable at runtime
// - Query size and depth limits: GraphQLParser.h (DEFAULT_QUERY_SIZE_LIMIT, DEFAULT_DEPTH_LIMIT)
// - Pagination limits: GraphQLToSparql.h (DEFAULT_MAX_RESULTS, DEFAULT_MAX_OFFSET)

// Thread-local depth tracking for recursive parsing
static thread_local size_t currentDepth = 0;
static thread_local size_t maxAllowedDepth = DEFAULT_DEPTH_LIMIT;

// Helper: Parse a selection set (recursive) with depth tracking
static bool parseSelectionSet(const std::string& content,
                              std::vector<Selection>& selections,
                              std::vector<GraphQLError>& errors) {
  // Check depth limit before recursing
  if (currentDepth >= maxAllowedDepth) {
    errors.push_back({.message = "Query depth limit exceeded (max " +
                                 std::to_string(maxAllowedDepth) + ")"});
    return false;
  }
  currentDepth++;

  size_t pos = 0;

  while (pos < content.size()) {
    pos = skipWhitespace(content, pos);
    if (pos >= content.size()) break;

    // Check for fragment spread (...FragmentName)
    if (pos + 2 < content.size() && content[pos] == '.' &&
        content[pos + 1] == '.' && content[pos + 2] == '.') {
      pos += 3;  // Skip "..."
      pos = skipWhitespace(content, pos);

      // Read fragment name
      std::string fragName = readIdentifier(content, pos);
      if (!fragName.empty()) {
        FragmentSpread spread;
        spread.name = fragName;
        selections.push_back(std::make_shared<FragmentSpread>(std::move(spread)));
        continue;
      }
    }

    // Read field/type name
    std::string name = readIdentifier(content, pos);
    if (name.empty()) {
      // Skip unknown characters
      pos++;
      continue;
    }

    Field field;
    field.name = name;

    pos = skipWhitespace(content, pos);

    // Check for alias (name followed by colon)
    if (pos < content.size() && content[pos] == ':') {
      field.alias = name;
      pos++;  // skip colon
      pos = skipWhitespace(content, pos);

      // Read actual field name
      field.name = readIdentifier(content, pos);
      pos = skipWhitespace(content, pos);
    }

    // Check for arguments (...)
    if (pos < content.size() && content[pos] == '(') {
      parseArguments(content, pos, field.arguments, errors);
    }

    pos = skipWhitespace(content, pos);

    // Check for nested selection set
    if (pos < content.size() && content[pos] == '{') {
      size_t closeBrace = findMatchingBrace(content, pos);
      if (closeBrace != std::string::npos) {
        std::string nestedContent = content.substr(pos + 1, closeBrace - pos - 1);
        std::vector<Selection> nestedSelections;
        if (!parseSelectionSet(nestedContent, nestedSelections, errors)) {
          currentDepth--;
          return false;  // Depth limit exceeded
        }
        field.selectionSet = std::move(nestedSelections);
        pos = closeBrace + 1;
      }
    }

    selections.push_back(std::make_shared<Field>(std::move(field)));
  }

  currentDepth--;
  return true;
}

// Backwards-compatible overload (no error collection)
static bool parseSelectionSet(const std::string& content,
                              std::vector<Selection>& selections) {
  std::vector<GraphQLError> errors;
  return parseSelectionSet(content, selections, errors);
}

// Helper: Parse a fragment definition from the document
// Returns the position after the fragment, or std::string::npos on error
static size_t parseFragmentDefinition(const std::string& doc, size_t startPos,
                                      FragmentDefinition& frag,
                                      std::vector<GraphQLError>& errors) {
  size_t pos = skipWhitespace(doc, startPos);

  // Must start with "fragment"
  if (doc.compare(pos, 8, "fragment") != 0 ||
      (pos + 8 < doc.size() && std::isalnum(doc[pos + 8]))) {
    errors.push_back({.message = "Expected 'fragment' keyword"});
    return std::string::npos;
  }
  pos += 8;
  pos = skipWhitespace(doc, pos);

  // Read fragment name
  frag.name = readIdentifier(doc, pos);
  if (frag.name.empty()) {
    errors.push_back({.message = "Expected fragment name"});
    return std::string::npos;
  }
  pos = skipWhitespace(doc, pos);

  // Must have "on" keyword
  if (doc.compare(pos, 2, "on") != 0 ||
      (pos + 2 < doc.size() && std::isalnum(doc[pos + 2]))) {
    errors.push_back({.message = "Expected 'on' keyword in fragment definition"});
    return std::string::npos;
  }
  pos += 2;
  pos = skipWhitespace(doc, pos);

  // Read type condition
  frag.typeCondition = readIdentifier(doc, pos);
  if (frag.typeCondition.empty()) {
    errors.push_back({.message = "Expected type condition in fragment definition"});
    return std::string::npos;
  }
  pos = skipWhitespace(doc, pos);

  // Find the selection set
  if (pos >= doc.size() || doc[pos] != '{') {
    errors.push_back({.message = "Missing opening brace in fragment definition"});
    return std::string::npos;
  }

  size_t closeBrace = findMatchingBrace(doc, pos);
  if (closeBrace == std::string::npos) {
    errors.push_back({.message = "Unmatched braces in fragment definition"});
    return std::string::npos;
  }

  // Extract and parse the selection set content
  std::string selectionContent = doc.substr(pos + 1, closeBrace - pos - 1);
  if (!parseSelectionSet(selectionContent, frag.selectionSet, errors)) {
    // Depth limit or other error occurred
    return std::string::npos;
  }

  return closeBrace + 1;
}

// Helper: Parse a single operation from the document
// Returns the position after the operation, or std::string::npos on error
static size_t parseOperation(const std::string& doc, size_t startPos,
                             Operation& op, std::vector<GraphQLError>& errors) {
  size_t pos = skipWhitespace(doc, startPos);
  if (pos >= doc.size()) {
    return std::string::npos;
  }

  op.type = OperationType::Query;

  // Check for operation keyword
  if (doc.compare(pos, 5, "query") == 0 &&
      (pos + 5 >= doc.size() || !std::isalnum(doc[pos + 5]))) {
    pos += 5;
    pos = skipWhitespace(doc, pos);
  } else if (doc.compare(pos, 8, "mutation") == 0 &&
             (pos + 8 >= doc.size() || !std::isalnum(doc[pos + 8]))) {
    op.type = OperationType::Mutation;
    pos += 8;
    pos = skipWhitespace(doc, pos);
  } else if (doc.compare(pos, 12, "subscription") == 0 &&
             (pos + 12 >= doc.size() || !std::isalnum(doc[pos + 12]))) {
    errors.push_back({.message = "Subscriptions are not supported"});
    return std::string::npos;
  } else if (doc[pos] != '{') {
    // Must be an anonymous query starting with { or have a keyword
    // Check if this looks like an identifier (possible operation name without keyword)
    std::string ident = readIdentifier(doc, pos);
    if (!ident.empty() && ident != "fragment") {
      // This might be a shorthand syntax error or unknown keyword
      pos = skipWhitespace(doc, pos);
      if (pos < doc.size() && doc[pos] == '{') {
        // Treat as named query without 'query' keyword (non-standard but handle gracefully)
        op.name = ident;
      } else {
        errors.push_back({.message = "Unknown operation type or syntax error"});
        return std::string::npos;
      }
    } else if (ident == "fragment") {
      // Fragment definitions are parsed separately in the main parse() function
      // Skip over them here in operation parsing - just find the end of this fragment
      size_t bracePos = doc.find('{', pos);
      if (bracePos != std::string::npos) {
        size_t closeBrace = findMatchingBrace(doc, bracePos);
        if (closeBrace != std::string::npos) {
          return closeBrace + 1;  // Return position after fragment
        }
      }
      return std::string::npos;
    }
  }

  // Check for operation name (before the opening brace or variable definitions)
  if (pos < doc.size() && doc[pos] != '{' && doc[pos] != '(') {
    std::string name = readIdentifier(doc, pos);
    if (!name.empty()) {
      op.name = name;
    }
    pos = skipWhitespace(doc, pos);
  }

  // Skip variable definitions ($name: Type, ...)
  if (pos < doc.size() && doc[pos] == '(') {
    size_t parenDepth = 1;
    pos++;
    while (pos < doc.size() && parenDepth > 0) {
      if (doc[pos] == '(') parenDepth++;
      else if (doc[pos] == ')') parenDepth--;
      pos++;
    }
    pos = skipWhitespace(doc, pos);
  }

  // Find the selection set
  if (pos >= doc.size() || doc[pos] != '{') {
    errors.push_back({.message = "Missing opening brace in operation"});
    return std::string::npos;
  }

  size_t closeBrace = findMatchingBrace(doc, pos);
  if (closeBrace == std::string::npos) {
    errors.push_back({.message = "Unmatched braces in operation"});
    return std::string::npos;
  }

  // Extract and parse the selection set content
  std::string selectionContent = doc.substr(pos + 1, closeBrace - pos - 1);
  if (!parseSelectionSet(selectionContent, op.selectionSet, errors)) {
    // Depth limit or other error occurred
    return std::string::npos;
  }

  return closeBrace + 1;
}

ParseResult GraphQLParser::parse(std::string_view query, size_t depthLimit,
                                 size_t querySizeLimit) {
  Document doc;
  std::vector<GraphQLError> errors;

  // Security: Enforce query size limit
  size_t effectiveSizeLimit =
      (querySizeLimit > 0) ? querySizeLimit : DEFAULT_QUERY_SIZE_LIMIT;
  if (query.size() > effectiveSizeLimit) {
    return std::vector<GraphQLError>{
        {.message = "Query too large (max " +
                    std::to_string(effectiveSizeLimit / 1024) + " KB)"}};
  }

  // Security: Set depth limit for this parse operation
  maxAllowedDepth = (depthLimit > 0) ? depthLimit : DEFAULT_DEPTH_LIMIT;
  currentDepth = 0;  // Reset depth counter

  std::string queryStr(query);

  // Remove leading/trailing whitespace
  size_t start = queryStr.find_first_not_of(" \t\n\r");
  size_t end = queryStr.find_last_not_of(" \t\n\r");
  if (start == std::string::npos) {
    return std::vector<GraphQLError>{{.message = "Empty query"}};
  }
  queryStr = queryStr.substr(start, end - start + 1);

  // Parse all operations in the document
  size_t pos = 0;
  while (pos < queryStr.size()) {
    pos = skipWhitespace(queryStr, pos);
    if (pos >= queryStr.size()) break;

    // Check for fragment definition
    if (queryStr.compare(pos, 8, "fragment") == 0 &&
        (pos + 8 >= queryStr.size() || !std::isalnum(queryStr[pos + 8]))) {
      FragmentDefinition frag;
      size_t nextPos = parseFragmentDefinition(queryStr, pos, frag, errors);

      if (nextPos == std::string::npos) {
        // Error already added to errors vector
        break;
      }

      // Only add non-empty fragments
      if (!frag.selectionSet.empty()) {
        doc.fragments.push_back(std::move(frag));
      }

      pos = nextPos;
      continue;
    }

    Operation op;
    size_t nextPos = parseOperation(queryStr, pos, op, errors);

    if (nextPos == std::string::npos) {
      // Error already added to errors vector
      break;
    }

    // Only add non-empty operations
    if (!op.selectionSet.empty()) {
      doc.operations.push_back(std::move(op));
    }

    pos = nextPos;
  }

  if (!errors.empty()) {
    return errors;
  }

  if (doc.operations.empty()) {
    return std::vector<GraphQLError>{{.message = "No valid operations found"}};
  }

  return doc;
}

ParseResult GraphQLParser::parseWithVariables(
    std::string_view query, const nlohmann::json& /*variables*/,
    std::optional<std::string_view> /*operationName*/) {
  // For now, just delegate to the basic parser
  // Variable substitution would be handled during execution
  return parse(query);
}

Document GraphQLParser::convertAST(const peg::ast& /*ast*/) {
  // Stub - not used with simplified parser
  return Document{};
}

std::vector<GraphQLError> GraphQLParser::validate(const Document& /*doc*/) {
  // Stub - basic validation would go here
  return {};
}

}  // namespace graphql
