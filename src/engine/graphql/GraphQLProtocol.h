// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifndef QLEVER_SRC_ENGINE_GRAPHQL_GRAPHQLPROTOCOL_H
#define QLEVER_SRC_ENGINE_GRAPHQL_GRAPHQLPROTOCOL_H

#ifdef QLEVER_GRAPHQL_SUPPORT

#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

#include "parser/graphql/GraphQLParser.h"
#include "util/http/UrlParser.h"
#include "util/http/beast.h"
#include "util/json.h"

namespace graphql {

/// A GraphQL operation extracted from an HTTP request
struct GraphQLOperation {
  /// The GraphQL query/mutation string
  std::string query;

  /// Optional operation name (for documents with multiple operations)
  std::optional<std::string> operationName;

  /// Variables passed with the request
  std::unordered_map<std::string, Value> variables;

  bool operator==(const GraphQLOperation&) const = default;
};

/// Parses HTTP requests containing GraphQL queries according to the
/// GraphQL over HTTP specification.
/// See: https://graphql.github.io/graphql-over-http/
class GraphQLProtocol {
 public:
  using RequestType =
      boost::beast::http::request<boost::beast::http::string_body>;

  /// Content types for GraphQL requests
  static constexpr std::string_view contentTypeJson = "application/json";
  static constexpr std::string_view contentTypeGraphQL = "application/graphql";

  /// Check if the request is a GraphQL request
  /// @param request The HTTP request to check
  /// @return true if the request is for the /graphql endpoint
  static bool isGraphQLRequest(const RequestType& request);

  /// Check if the request is a GraphQL config request
  /// @param request The HTTP request to check
  /// @return true if the request is for the /graphql/config endpoint
  static bool isGraphQLConfigRequest(const RequestType& request);

  /// Extract access token from request (from Authorization header or URL param)
  /// @param request The HTTP request
  /// @return The access token if present, std::nullopt otherwise
  static std::optional<std::string> extractAccessToken(
      const RequestType& request);

  /// Extract timeout parameter from request URL
  /// @param request The HTTP request
  /// @return The timeout string if present, std::nullopt otherwise
  static std::optional<std::string> extractTimeout(const RequestType& request);

  /// Parse an HTTP request containing a GraphQL query
  /// @param request The HTTP request
  /// @return The parsed GraphQL operation or an error
  static std::variant<GraphQLOperation, std::vector<GraphQLError>>
  parseHttpRequest(const RequestType& request);

 private:
  /// Parse a GET request with GraphQL query in URL parameters
  static std::variant<GraphQLOperation, std::vector<GraphQLError>> parseGET(
      const RequestType& request);

  /// Parse a POST request with JSON body
  static std::variant<GraphQLOperation, std::vector<GraphQLError>>
  parseJSONPost(const RequestType& request);

  /// Parse a POST request with application/graphql content type
  static std::variant<GraphQLOperation, std::vector<GraphQLError>>
  parseGraphQLPost(const RequestType& request);

  /// Parse variables from JSON
  static std::unordered_map<std::string, Value> parseVariables(
      const nlohmann::json& varsJson);

  /// Convert JSON value to GraphQL Value
  static Value jsonToValue(const nlohmann::json& json);
};

}  // namespace graphql

#endif  // QLEVER_GRAPHQL_SUPPORT
#endif  // QLEVER_SRC_ENGINE_GRAPHQL_GRAPHQLPROTOCOL_H
