// Copyright 2025, University of Freiburg
// Chair of Algorithms and Data Structures
// Authors: GraphQL support for QLever

#ifdef QLEVER_GRAPHQL_SUPPORT

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "engine/graphql/GraphQLProtocol.h"

using namespace graphql;
namespace http = boost::beast::http;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;

// Helper to create an HTTP request
http::request<http::string_body> makeRequest(
    http::verb method, std::string_view target, std::string_view body = "",
    std::string_view contentType = "") {
  http::request<http::string_body> req{method, std::string(target), 11};
  req.body() = std::string(body);
  if (!contentType.empty()) {
    req.set(http::field::content_type, std::string(contentType));
  }
  req.prepare_payload();
  return req;
}

// ============================================================================
// isGraphQLRequest Tests
// ============================================================================

TEST(GraphQLProtocolTest, IsGraphQLRequest_ValidPath) {
  auto req = makeRequest(http::verb::post, "/graphql");
  EXPECT_TRUE(GraphQLProtocol::isGraphQLRequest(req));
}

TEST(GraphQLProtocolTest, IsGraphQLRequest_ValidPathWithSlash) {
  auto req = makeRequest(http::verb::post, "/graphql/");
  EXPECT_TRUE(GraphQLProtocol::isGraphQLRequest(req));
}

TEST(GraphQLProtocolTest, IsGraphQLRequest_ValidPathWithQueryParams) {
  auto req = makeRequest(http::verb::get, "/graphql?query={Person{id}}");
  EXPECT_TRUE(GraphQLProtocol::isGraphQLRequest(req));
}

TEST(GraphQLProtocolTest, IsGraphQLRequest_InvalidPath) {
  auto req = makeRequest(http::verb::post, "/sparql");
  EXPECT_FALSE(GraphQLProtocol::isGraphQLRequest(req));
}

TEST(GraphQLProtocolTest, IsGraphQLRequest_RootPath) {
  auto req = makeRequest(http::verb::post, "/");
  EXPECT_FALSE(GraphQLProtocol::isGraphQLRequest(req));
}

// ============================================================================
// GET Request Tests
// ============================================================================

TEST(GraphQLProtocolTest, ParseGET_SimpleQuery) {
  auto req = makeRequest(
      http::verb::get,
      "/graphql?query=%7BPerson%7Bid%7D%7D");  // {Person{id}}

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<GraphQLOperation>(result));
  const auto& op = std::get<GraphQLOperation>(result);
  EXPECT_EQ(op.query, "{Person{id}}");
  EXPECT_FALSE(op.operationName.has_value());
  EXPECT_TRUE(op.variables.empty());
}

TEST(GraphQLProtocolTest, ParseGET_WithOperationName) {
  auto req = makeRequest(
      http::verb::get,
      "/graphql?query=%7BPerson%7Bid%7D%7D&operationName=GetPeople");

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<GraphQLOperation>(result));
  const auto& op = std::get<GraphQLOperation>(result);
  EXPECT_EQ(op.query, "{Person{id}}");
  EXPECT_EQ(op.operationName, "GetPeople");
}

TEST(GraphQLProtocolTest, ParseGET_WithVariables) {
  auto req = makeRequest(
      http::verb::get,
      "/graphql?query=%7BPerson%7Bid%7D%7D"
      "&variables=%7B%22limit%22%3A10%7D");  // {"limit":10}

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<GraphQLOperation>(result));
  const auto& op = std::get<GraphQLOperation>(result);
  ASSERT_THAT(op.variables, SizeIs(1));
  EXPECT_TRUE(op.variables.count("limit") > 0);
  EXPECT_EQ(op.variables.at("limit").asInt(), 10);
}

TEST(GraphQLProtocolTest, ParseGET_MissingQuery) {
  auto req = makeRequest(http::verb::get, "/graphql");

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<std::vector<GraphQLError>>(result));
  const auto& errors = std::get<std::vector<GraphQLError>>(result);
  ASSERT_THAT(errors, SizeIs(1));
  EXPECT_THAT(errors[0].message, ::testing::HasSubstr("query"));
}

// ============================================================================
// POST JSON Request Tests
// ============================================================================

TEST(GraphQLProtocolTest, ParsePOST_JSON_SimpleQuery) {
  auto req = makeRequest(
      http::verb::post, "/graphql",
      R"({"query": "{ Person { id } }"})",
      "application/json");

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<GraphQLOperation>(result));
  const auto& op = std::get<GraphQLOperation>(result);
  EXPECT_EQ(op.query, "{ Person { id } }");
}

TEST(GraphQLProtocolTest, ParsePOST_JSON_WithOperationName) {
  auto req = makeRequest(
      http::verb::post, "/graphql",
      R"({"query": "query GetPeople { Person { id } }", "operationName": "GetPeople"})",
      "application/json");

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<GraphQLOperation>(result));
  const auto& op = std::get<GraphQLOperation>(result);
  EXPECT_EQ(op.operationName, "GetPeople");
}

TEST(GraphQLProtocolTest, ParsePOST_JSON_WithVariables) {
  auto req = makeRequest(
      http::verb::post, "/graphql",
      R"({
        "query": "query ($limit: Int) { Person(first: $limit) { id } }",
        "variables": {"limit": 10, "name": "Einstein", "active": true}
      })",
      "application/json");

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<GraphQLOperation>(result));
  const auto& op = std::get<GraphQLOperation>(result);
  ASSERT_THAT(op.variables, SizeIs(3));
  EXPECT_EQ(op.variables.at("limit").asInt(), 10);
  EXPECT_EQ(op.variables.at("name").asString(), "Einstein");
  EXPECT_EQ(op.variables.at("active").asBool(), true);
}

TEST(GraphQLProtocolTest, ParsePOST_JSON_ComplexVariables) {
  auto req = makeRequest(
      http::verb::post, "/graphql",
      R"({
        "query": "query { Person { id } }",
        "variables": {
          "filter": {"name": {"contains": "Einstein"}},
          "ids": [1, 2, 3]
        }
      })",
      "application/json");

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<GraphQLOperation>(result));
  const auto& op = std::get<GraphQLOperation>(result);
  ASSERT_THAT(op.variables, SizeIs(2));

  // Check object variable
  EXPECT_EQ(op.variables.at("filter").type, Value::Type::Object);

  // Check list variable
  EXPECT_EQ(op.variables.at("ids").type, Value::Type::List);
  const auto& idsList = op.variables.at("ids").asList();
  ASSERT_THAT(idsList, SizeIs(3));
}

TEST(GraphQLProtocolTest, ParsePOST_JSON_MissingQuery) {
  auto req = makeRequest(
      http::verb::post, "/graphql",
      R"({"operationName": "Test"})",
      "application/json");

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<std::vector<GraphQLError>>(result));
}

TEST(GraphQLProtocolTest, ParsePOST_JSON_InvalidJSON) {
  auto req = makeRequest(
      http::verb::post, "/graphql",
      "{ invalid json }",
      "application/json");

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<std::vector<GraphQLError>>(result));
  const auto& errors = std::get<std::vector<GraphQLError>>(result);
  EXPECT_THAT(errors[0].message, ::testing::HasSubstr("Invalid JSON"));
}

// ============================================================================
// POST application/graphql Request Tests
// ============================================================================

TEST(GraphQLProtocolTest, ParsePOST_GraphQL_SimpleQuery) {
  auto req = makeRequest(
      http::verb::post, "/graphql",
      "{ Person { id name } }",
      "application/graphql");

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<GraphQLOperation>(result));
  const auto& op = std::get<GraphQLOperation>(result);
  EXPECT_EQ(op.query, "{ Person { id name } }");
}

TEST(GraphQLProtocolTest, ParsePOST_GraphQL_WithURLParams) {
  auto req = makeRequest(
      http::verb::post, "/graphql?operationName=GetPeople",
      "query GetPeople { Person { id } }",
      "application/graphql");

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<GraphQLOperation>(result));
  const auto& op = std::get<GraphQLOperation>(result);
  EXPECT_EQ(op.query, "query GetPeople { Person { id } }");
  EXPECT_EQ(op.operationName, "GetPeople");
}

TEST(GraphQLProtocolTest, ParsePOST_GraphQL_EmptyBody) {
  auto req = makeRequest(
      http::verb::post, "/graphql",
      "",
      "application/graphql");

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<std::vector<GraphQLError>>(result));
  const auto& errors = std::get<std::vector<GraphQLError>>(result);
  EXPECT_THAT(errors[0].message, ::testing::HasSubstr("Empty"));
}

// ============================================================================
// Unsupported Methods/Content Types Tests
// ============================================================================

TEST(GraphQLProtocolTest, ParsePUT_Unsupported) {
  auto req = makeRequest(http::verb::put, "/graphql", "{}", "application/json");

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<std::vector<GraphQLError>>(result));
  const auto& errors = std::get<std::vector<GraphQLError>>(result);
  EXPECT_THAT(errors[0].message, ::testing::HasSubstr("GET and POST"));
}

TEST(GraphQLProtocolTest, ParsePOST_UnsupportedContentType) {
  auto req = makeRequest(
      http::verb::post, "/graphql",
      "<query>test</query>",
      "application/xml");

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<std::vector<GraphQLError>>(result));
  const auto& errors = std::get<std::vector<GraphQLError>>(result);
  EXPECT_THAT(errors[0].message, ::testing::HasSubstr("Content-Type"));
}

// ============================================================================
// Default Content Type Tests
// ============================================================================

TEST(GraphQLProtocolTest, ParsePOST_NoContentType_DefaultsToJSON) {
  auto req = makeRequest(
      http::verb::post, "/graphql",
      R"({"query": "{ Person { id } }"})",
      "");  // No content type

  auto result = GraphQLProtocol::parseHttpRequest(req);

  ASSERT_TRUE(std::holds_alternative<GraphQLOperation>(result));
  const auto& op = std::get<GraphQLOperation>(result);
  EXPECT_EQ(op.query, "{ Person { id } }");
}

// ============================================================================
// Config Endpoint Detection Tests
// ============================================================================

TEST(GraphQLProtocolTest, IsGraphQLConfigRequest_ConfigPath) {
  auto req = makeRequest(http::verb::get, "/graphql/config", "", "");
  EXPECT_TRUE(GraphQLProtocol::isGraphQLConfigRequest(req));
}

TEST(GraphQLProtocolTest, IsGraphQLConfigRequest_ConfigPathWithSlash) {
  auto req = makeRequest(http::verb::get, "/graphql/config/", "", "");
  EXPECT_TRUE(GraphQLProtocol::isGraphQLConfigRequest(req));
}

TEST(GraphQLProtocolTest, IsGraphQLConfigRequest_NotConfigPath) {
  auto req = makeRequest(http::verb::get, "/graphql", "", "");
  EXPECT_FALSE(GraphQLProtocol::isGraphQLConfigRequest(req));
}

TEST(GraphQLProtocolTest, IsGraphQLConfigRequest_OtherPath) {
  auto req = makeRequest(http::verb::get, "/sparql", "", "");
  EXPECT_FALSE(GraphQLProtocol::isGraphQLConfigRequest(req));
}

TEST(GraphQLProtocolTest, IsGraphQLRequest_ExcludesConfigPath) {
  // /graphql/config should NOT be treated as a regular GraphQL request
  auto req = makeRequest(http::verb::get, "/graphql/config", "", "");
  EXPECT_FALSE(GraphQLProtocol::isGraphQLRequest(req));
}

// ============================================================================
// Access Token Extraction Tests
// ============================================================================

TEST(GraphQLProtocolTest, ExtractAccessToken_FromAuthorizationHeader) {
  auto req = makeRequest(http::verb::get, "/graphql", "", "");
  req.set(http::field::authorization, "Bearer my-secret-token");

  auto token = GraphQLProtocol::extractAccessToken(req);
  ASSERT_TRUE(token.has_value());
  EXPECT_EQ(token.value(), "my-secret-token");
}

TEST(GraphQLProtocolTest, ExtractAccessToken_FromURLParameter) {
  auto req = makeRequest(http::verb::get, "/graphql?access-token=url-token", "", "");

  auto token = GraphQLProtocol::extractAccessToken(req);
  ASSERT_TRUE(token.has_value());
  EXPECT_EQ(token.value(), "url-token");
}

TEST(GraphQLProtocolTest, ExtractAccessToken_HeaderPrecedence) {
  // When both header and URL param are provided, header takes precedence
  auto req = makeRequest(http::verb::get, "/graphql?access-token=url-token", "", "");
  req.set(http::field::authorization, "Bearer header-token");

  auto token = GraphQLProtocol::extractAccessToken(req);
  ASSERT_TRUE(token.has_value());
  EXPECT_EQ(token.value(), "header-token");
}

TEST(GraphQLProtocolTest, ExtractAccessToken_NoToken) {
  auto req = makeRequest(http::verb::get, "/graphql", "", "");

  auto token = GraphQLProtocol::extractAccessToken(req);
  EXPECT_FALSE(token.has_value());
}

TEST(GraphQLProtocolTest, ExtractAccessToken_InvalidAuthorizationHeader) {
  // Authorization header without "Bearer " prefix is ignored
  auto req = makeRequest(http::verb::get, "/graphql", "", "");
  req.set(http::field::authorization, "Basic dXNlcjpwYXNz");

  auto token = GraphQLProtocol::extractAccessToken(req);
  EXPECT_FALSE(token.has_value());
}

// ============================================================================
// Timeout Extraction Tests
// ============================================================================

TEST(GraphQLProtocolTest, ExtractTimeout_FromURLParameter) {
  auto req = makeRequest(http::verb::get, "/graphql?timeout=30s", "", "");

  auto timeout = GraphQLProtocol::extractTimeout(req);
  ASSERT_TRUE(timeout.has_value());
  EXPECT_EQ(timeout.value(), "30s");
}

TEST(GraphQLProtocolTest, ExtractTimeout_NoTimeout) {
  auto req = makeRequest(http::verb::get, "/graphql", "", "");

  auto timeout = GraphQLProtocol::extractTimeout(req);
  EXPECT_FALSE(timeout.has_value());
}

TEST(GraphQLProtocolTest, ExtractTimeout_WithOtherParams) {
  auto req = makeRequest(http::verb::get,
      "/graphql?access-token=abc&timeout=60s&other=value", "", "");

  auto timeout = GraphQLProtocol::extractTimeout(req);
  ASSERT_TRUE(timeout.has_value());
  EXPECT_EQ(timeout.value(), "60s");
}

#endif  // QLEVER_GRAPHQL_SUPPORT
