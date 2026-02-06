// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifdef QLEVER_GRAPHQL_SUPPORT

#include "engine/graphql/GraphQLProtocol.h"

#include <boost/url.hpp>

#include "backports/StartsWithAndEndsWith.h"
#include "util/http/UrlParser.h"

namespace http = boost::beast::http;

namespace graphql {

// ____________________________________________________________________________
bool GraphQLProtocol::isGraphQLRequest(const RequestType& request) {
  // Check if the path starts with /graphql
  std::string target{request.target()};

  // First try parsing with origin_form (handles paths with query strings)
  auto urlResult = boost::urls::parse_origin_form(target);
  if (urlResult.has_error()) {
    // Fallback: just check the target string directly
    return ql::starts_with(target, "/graphql");
  }
  boost::url url = urlResult.value();
  std::string path = url.path();

  // Accept /graphql or /graphql/ (with or without query parameters)
  // The path() method excludes query params, so this handles both cases
  // But exclude /graphql/config which is handled separately
  if (path == "/graphql/config" || path == "/graphql/config/") {
    return false;
  }
  return path == "/graphql" || path == "/graphql/";
}

// ____________________________________________________________________________
bool GraphQLProtocol::isGraphQLConfigRequest(const RequestType& request) {
  std::string target{request.target()};

  auto urlResult = boost::urls::parse_origin_form(target);
  if (urlResult.has_error()) {
    return ql::starts_with(target, "/graphql/config");
  }
  boost::url url = urlResult.value();
  std::string path = url.path();

  return path == "/graphql/config" || path == "/graphql/config/";
}

// ____________________________________________________________________________
std::optional<std::string> GraphQLProtocol::extractAccessToken(
    const RequestType& request) {
  // Check Authorization header first (Bearer token)
  std::string_view authorization = request[http::field::authorization];
  std::optional<std::string> tokenFromHeader;
  if (!authorization.empty()) {
    const std::string prefix = "Bearer ";
    if (ql::starts_with(authorization, prefix)) {
      authorization.remove_prefix(prefix.length());
      tokenFromHeader = std::string(authorization);
    }
  }

  // Check URL parameter
  std::optional<std::string> tokenFromParam;
  std::string target{request.target()};
  auto urlResult = boost::urls::parse_origin_form(target);
  if (!urlResult.has_error()) {
    boost::url url = urlResult.value();
    auto params = url.params();
    auto it = params.find("access-token");
    if (it != params.end()) {
      tokenFromParam = std::string((*it).value);
    }
  }

  // If both are specified, prefer header (consistent with SPARQL behavior)
  if (tokenFromHeader) {
    return tokenFromHeader;
  }
  return tokenFromParam;
}

// ____________________________________________________________________________
std::optional<std::string> GraphQLProtocol::extractTimeout(
    const RequestType& request) {
  std::string target{request.target()};
  auto urlResult = boost::urls::parse_origin_form(target);
  if (!urlResult.has_error()) {
    boost::url url = urlResult.value();
    auto params = url.params();
    auto it = params.find("timeout");
    if (it != params.end()) {
      return std::string((*it).value);
    }
  }
  return std::nullopt;
}

// ____________________________________________________________________________
std::variant<GraphQLOperation, std::vector<GraphQLError>>
GraphQLProtocol::parseHttpRequest(const RequestType& request) {
  // According to GraphQL over HTTP spec:
  // - GET requests: query in URL parameters
  // - POST with application/json: JSON body with query, variables, operationName
  // - POST with application/graphql: raw query in body

  if (request.method() == http::verb::get) {
    return parseGET(request);
  }

  if (request.method() == http::verb::post) {
    std::string_view contentType =
        request.base()[http::field::content_type];

    if (ql::starts_with(contentType, contentTypeJson)) {
      return parseJSONPost(request);
    }

    if (ql::starts_with(contentType, contentTypeGraphQL)) {
      return parseGraphQLPost(request);
    }

    // Default to JSON for POST if no content-type specified
    if (contentType.empty()) {
      return parseJSONPost(request);
    }

    return std::vector<GraphQLError>{
        {.message = "Unsupported Content-Type for GraphQL: " +
                    std::string(contentType),
         .extensions = {{"code", "UNSUPPORTED_CONTENT_TYPE"}}}};
  }

  return std::vector<GraphQLError>{
      {.message = "GraphQL only supports GET and POST requests",
       .extensions = {{"code", "METHOD_NOT_ALLOWED"}}}};
}

// ____________________________________________________________________________
std::variant<GraphQLOperation, std::vector<GraphQLError>>
GraphQLProtocol::parseGET(const RequestType& request) {
  // Parse URL parameters
  std::string target{request.target()};
  auto urlResult = boost::urls::parse_origin_form(target);
  if (urlResult.has_error()) {
    return std::vector<GraphQLError>{
        {.message = "Invalid URL",
         .extensions = {{"code", "INVALID_URL"}}}};
  }

  boost::url url = urlResult.value();
  auto params = ad_utility::url_parser::paramsToMap(url.params());

  // Extract query parameter (required)
  auto queryIt = params.find("query");
  if (queryIt == params.end() || queryIt->second.empty()) {
    return std::vector<GraphQLError>{
        {.message = "Missing required 'query' parameter in GET request",
         .extensions = {{"code", "MISSING_QUERY"}}}};
  }

  GraphQLOperation operation;
  operation.query = queryIt->second[0];

  // Extract optional operationName
  auto opNameIt = params.find("operationName");
  if (opNameIt != params.end() && !opNameIt->second.empty()) {
    operation.operationName = opNameIt->second[0];
  }

  // Extract optional variables (JSON-encoded)
  auto varsIt = params.find("variables");
  if (varsIt != params.end() && !varsIt->second.empty()) {
    try {
      auto varsJson = nlohmann::json::parse(varsIt->second[0]);
      operation.variables = parseVariables(varsJson);
    } catch (const nlohmann::json::exception& e) {
      return std::vector<GraphQLError>{
          {.message = "Invalid JSON in 'variables' parameter: " +
                      std::string(e.what()),
           .extensions = {{"code", "INVALID_VARIABLES"}}}};
    }
  }

  return operation;
}

// ____________________________________________________________________________
std::variant<GraphQLOperation, std::vector<GraphQLError>>
GraphQLProtocol::parseJSONPost(const RequestType& request) {
  // Parse JSON body
  nlohmann::json body;
  try {
    body = nlohmann::json::parse(request.body());
  } catch (const nlohmann::json::exception& e) {
    return std::vector<GraphQLError>{
        {.message = "Invalid JSON in request body: " + std::string(e.what()),
         .extensions = {{"code", "INVALID_JSON"}}}};
  }

  // Extract query (required)
  if (!body.contains("query") || !body["query"].is_string()) {
    return std::vector<GraphQLError>{
        {.message = "Missing required 'query' field in JSON body",
         .extensions = {{"code", "MISSING_QUERY"}}}};
  }

  GraphQLOperation operation;
  operation.query = body["query"].get<std::string>();

  // Extract optional operationName
  if (body.contains("operationName") && body["operationName"].is_string()) {
    operation.operationName = body["operationName"].get<std::string>();
  }

  // Extract optional variables
  if (body.contains("variables") && body["variables"].is_object()) {
    operation.variables = parseVariables(body["variables"]);
  }

  return operation;
}

// ____________________________________________________________________________
std::variant<GraphQLOperation, std::vector<GraphQLError>>
GraphQLProtocol::parseGraphQLPost(const RequestType& request) {
  // For application/graphql, the body is the raw query string
  // Variables and operationName can come from URL parameters

  if (request.body().empty()) {
    return std::vector<GraphQLError>{
        {.message = "Empty GraphQL query in request body",
         .extensions = {{"code", "EMPTY_QUERY"}}}};
  }

  GraphQLOperation operation;
  operation.query = request.body();

  // Check URL parameters for variables and operationName
  std::string target{request.target()};
  auto urlResult = boost::urls::parse_origin_form(target);
  if (!urlResult.has_error()) {
    boost::url url = urlResult.value();
    auto params = ad_utility::url_parser::paramsToMap(url.params());

    auto opNameIt = params.find("operationName");
    if (opNameIt != params.end() && !opNameIt->second.empty()) {
      operation.operationName = opNameIt->second[0];
    }

    auto varsIt = params.find("variables");
    if (varsIt != params.end() && !varsIt->second.empty()) {
      try {
        auto varsJson = nlohmann::json::parse(varsIt->second[0]);
        operation.variables = parseVariables(varsJson);
      } catch (const nlohmann::json::exception&) {
        // Ignore invalid variables in URL for application/graphql
      }
    }
  }

  return operation;
}

// ____________________________________________________________________________
std::unordered_map<std::string, Value> GraphQLProtocol::parseVariables(
    const nlohmann::json& varsJson) {
  std::unordered_map<std::string, Value> result;

  if (!varsJson.is_object()) {
    return result;
  }

  for (auto& [key, value] : varsJson.items()) {
    result[key] = jsonToValue(value);
  }

  return result;
}

// ____________________________________________________________________________
Value GraphQLProtocol::jsonToValue(const nlohmann::json& json) {
  if (json.is_null()) {
    return Value::makeNull();
  }
  if (json.is_boolean()) {
    return Value::makeBool(json.get<bool>());
  }
  if (json.is_number_integer()) {
    return Value::makeInt(json.get<int64_t>());
  }
  if (json.is_number_float()) {
    return Value::makeFloat(json.get<double>());
  }
  if (json.is_string()) {
    return Value::makeString(json.get<std::string>());
  }
  if (json.is_array()) {
    std::vector<Value> values;
    for (const auto& item : json) {
      values.push_back(jsonToValue(item));
    }
    return Value::makeList(std::move(values));
  }
  if (json.is_object()) {
    std::vector<std::pair<std::string, Value>> fields;
    for (auto& [key, value] : json.items()) {
      fields.emplace_back(key, jsonToValue(value));
    }
    return Value::makeObject(std::move(fields));
  }

  // Default to null for unknown types
  return Value::makeNull();
}

}  // namespace graphql

#endif  // QLEVER_GRAPHQL_SUPPORT
