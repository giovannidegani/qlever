# GraphQL Support in QLever

QLever provides native GraphQL support, allowing you to query RDF data using GraphQL syntax. This is an alternative to SPARQL that may be more familiar to developers coming from web application backgrounds.

## Overview

The GraphQL endpoint automatically generates a schema from your RDF data, mapping:
- RDF classes to GraphQL types
- RDF properties to GraphQL fields
- XSD datatypes to GraphQL scalars

## Endpoint

GraphQL requests are served at the `/graphql` endpoint:

```
POST http://your-qlever-server:port/graphql
GET  http://your-qlever-server:port/graphql?query=...
```

## Quick Start

### Basic Query

```graphql
query {
  Person {
    id
    name
    birthDate
  }
}
```

This translates to the SPARQL equivalent:
```sparql
SELECT ?person ?name ?birthDate WHERE {
  ?person a <http://schema.org/Person> .
  ?person <http://schema.org/name> ?name .
  OPTIONAL { ?person <http://schema.org/birthDate> ?birthDate }
}
```

### Using curl

**POST request (recommended):**
```bash
curl -X POST http://localhost:7001/graphql \
  -H "Content-Type: application/json" \
  -d '{"query": "{ Person { id name } }"}'
```

**GET request:**
```bash
curl "http://localhost:7001/graphql?query=%7B%20Person%20%7B%20id%20name%20%7D%20%7D"
```

## Query Features

### Field Selection

Select specific fields to include in the response:

```graphql
query {
  Person {
    id
    name
    birthDate
    email
  }
}
```

### Aliases

Rename fields in the response:

```graphql
query {
  Person {
    identifier: id
    fullName: name
  }
}
```

### Nested Fields (Relationships)

Query related entities:

```graphql
query {
  Person {
    id
    name
    knows {
      id
      name
    }
    worksFor {
      id
      name
    }
  }
}
```

### Filtering

Filter results using the `filter` argument:

```graphql
query {
  Person(filter: { name: { contains: "Einstein" } }) {
    id
    name
  }
}
```

**Available filter operators:**

| Type | Operators |
|------|-----------|
| String | `eq`, `ne`, `contains`, `startsWith`, `endsWith`, `in` |
| Numeric | `eq`, `ne`, `gt`, `gte`, `lt`, `lte`, `in` |
| Boolean | `eq` |
| ID | `eq`, `ne`, `in` |

### Logical Operators

Combine filters with AND, OR, and NOT:

```graphql
# AND (implicit - multiple conditions)
query {
  Person(filter: {
    name: { contains: "Einstein" }
    birthDate: { gt: "1800-01-01" }
  }) {
    id name
  }
}

# OR
query {
  Person(filter: {
    OR: [
      { name: { eq: "Einstein" } },
      { name: { eq: "Bohr" } }
    ]
  }) {
    id name
  }
}

# NOT
query {
  Person(filter: {
    NOT: { name: { contains: "Unknown" } }
  }) {
    id name
  }
}
```

### Pagination

Limit and offset results:

```graphql
query {
  Person(first: 10, offset: 20) {
    id
    name
  }
}
```

**Pagination limits:**
- Maximum `first`/`limit`: 10,000
- Maximum `offset`: 1,000,000

### Ordering

Sort results:

```graphql
query {
  Person(orderBy: "name", orderDirection: ASC) {
    id
    name
  }
}
```

### Fragments

Reuse field selections:

```graphql
fragment PersonFields on Person {
  id
  name
  birthDate
}

query {
  Person {
    ...PersonFields
    knows {
      ...PersonFields
    }
  }
}
```

### Multiple Operations

Define multiple operations in a document:

```graphql
query GetPeople {
  Person { id name }
}

query GetOrganizations {
  Organization { id name }
}
```

When using multiple operations, specify which to execute:
```bash
curl -X POST http://localhost:7001/graphql \
  -H "Content-Type: application/json" \
  -d '{"query": "...", "operationName": "GetPeople"}'
```

## Mutations (Experimental)

QLever includes experimental support for GraphQL mutations. Mutations allow you to create, update, and delete RDF data using GraphQL syntax.

> **Note**: Full mutation execution requires SPARQL Update support in QLever. Currently, mutations are translated to SPARQL Update statements but execution may be limited depending on your QLever configuration.

### Available Mutation Types

| Mutation | Description |
|----------|-------------|
| `create{Type}` | Create a new entity |
| `update{Type}` | Update an existing entity |
| `delete{Type}` | Delete an entity |
| `upsert{Type}` | Create or update an entity |
| `create{Types}` | Batch create multiple entities |
| `delete{Types}` | Batch delete multiple entities |

### Create Mutation

```graphql
mutation {
  createPerson(input: {
    name: "Alice Smith"
    email: "alice@example.com"
    age: 30
  }) {
    id
    name
    email
  }
}
```

Generated SPARQL Update:
```sparql
INSERT DATA {
  <http://example.org/Person/uuid-123> a <http://example.org/Person> .
  <http://example.org/Person/uuid-123> <http://example.org/name> "Alice Smith" .
  <http://example.org/Person/uuid-123> <http://example.org/email> "alice@example.com" .
  <http://example.org/Person/uuid-123> <http://example.org/age> "30"^^xsd:integer .
}
```

### Update Mutation

```graphql
mutation {
  updatePerson(
    id: "http://example.org/Person/123"
    input: {
      name: "Alice Johnson"
    }
  ) {
    id
    name
  }
}
```

Generated SPARQL Update:
```sparql
DELETE {
  <http://example.org/Person/123> <http://example.org/name> ?old_name .
}
INSERT {
  <http://example.org/Person/123> <http://example.org/name> "Alice Johnson" .
}
WHERE {
  <http://example.org/Person/123> a <http://example.org/Person> .
}
```

### Delete Mutation

```graphql
mutation {
  deletePerson(id: "http://example.org/Person/123") {
    success
  }
}
```

### Batch Create

```graphql
mutation {
  createPersons(input: [
    { name: "Alice" },
    { name: "Bob" },
    { name: "Charlie" }
  ]) {
    id
    name
  }
}
```

### Nested Mutations (Relations)

For types with relationships, you can use nested mutation operations:

```graphql
mutation {
  createPerson(input: {
    name: "Alice"
    worksFor: {
      connect: ["http://example.org/Organization/1"]
    }
    knows: {
      create: [{ name: "Bob" }]
    }
  }) {
    id
    name
    worksFor { name }
    knows { name }
  }
}
```

**Relation operations:**
- `connect`: Link to existing entities by ID
- `disconnect`: Remove links to entities
- `create`: Create new related entities
- `delete`: Delete related entities
- `set`: Replace all connections

### IRI Generation

New entities receive automatically generated IRIs. The strategy can be configured:

| Strategy | Description | Example |
|----------|-------------|---------|
| UUID (default) | Random UUID-based IRI | `http://example.org/Person/a1b2c3d4-...` |
| UserProvided | Use the `id` field from input | `http://example.org/custom-id` |
| Template | Pattern-based generation | `http://example.org/Person/{email}` |

### Versioning and Timestamps

When enabled, mutations automatically track:

- `_createdAt`: ISO 8601 timestamp when entity was created
- `_updatedAt`: ISO 8601 timestamp when entity was last modified
- `_version`: Integer version for optimistic locking

```graphql
mutation {
  updatePerson(
    id: "http://example.org/Person/123"
    input: {
      name: "New Name"
      _version: 1  # Fails if current version is not 1
    }
  ) {
    id
    _version  # Returns 2 after update
    _updatedAt
  }
}
```

### Input Validation

Mutation inputs are validated before execution:

- **Required fields**: Must be present for create mutations
- **Type validation**: Values must match expected scalar types
- **Reference validation**: Optionally verify linked entities exist
- **Size limits**: Maximum batch sizes and string lengths

Validation errors are returned in the standard GraphQL error format:

```json
{
  "errors": [
    {
      "message": "Required field 'name' is missing",
      "extensions": {
        "code": "REQUIRED_FIELD_MISSING",
        "field": "name"
      }
    }
  ]
}
```

### Mutation Limitations

Current limitations of GraphQL mutations in QLever:

| Feature | Status |
|---------|--------|
| Single entity mutations | Supported |
| Batch mutations | Supported |
| Nested create | Supported |
| Connect/Disconnect | Supported |
| SPARQL Update execution | Requires configuration |
| Transactions | Not supported |
| Subscriptions | Not supported |

## Introspection

### Schema Introspection

Get the full schema:

```graphql
query {
  __schema {
    types {
      name
      kind
      fields {
        name
        type { name }
      }
    }
  }
}
```

### Type Introspection

Get details about a specific type:

```graphql
query {
  __type(name: "Person") {
    name
    kind
    fields {
      name
      type { name kind }
    }
  }
}
```

### Type Name in Results

Get the type name in query results:

```graphql
query {
  Person {
    __typename
    id
    name
  }
}
```

## Response Format

GraphQL responses follow the standard format:

**Success:**
```json
{
  "data": {
    "Person": [
      { "id": "wd:Q937", "name": "Einstein" },
      { "id": "wd:Q34969", "name": "Bohr" }
    ]
  }
}
```

**Error:**
```json
{
  "data": null,
  "errors": [
    {
      "message": "Unknown field 'foo' on type 'Person'",
      "locations": [{ "line": 2, "column": 5 }]
    }
  ]
}
```

## Security

The GraphQL endpoint includes several security features:

| Feature | Default Limit | Configurable |
|---------|---------------|--------------|
| Query size | 100 KB | Yes |
| Query depth | 25 levels | Yes |
| Results per query | 10,000 | Yes |
| Offset | 1,000,000 | Yes |
| Fragment cycles | Detected and rejected | No |

All limits can be adjusted programmatically at runtime without recompiling:

```cpp
// Parser limits (query size and depth)
auto result = GraphQLParser::parse(query, depthLimit, querySizeLimit);

// Translation limits (pagination)
TranslationConfig config;
config.maxResults = 5000;   // Limit results per query
config.maxOffset = 50000;   // Limit offset for pagination
GraphQLToSparql translator(schema, config);
```

## Configuration

GraphQL support is enabled by default. To disable it during build:

```bash
cmake .. -DGRAPHQL_SUPPORT=OFF
```

## Schema Generation

The GraphQL schema is automatically generated from your RDF data by:

1. Discovering classes with `rdf:type` relationships
2. Finding properties used by instances of each class
3. Inferring scalar types from XSD datatypes
4. Detecting object relationships between types

The schema is cached after the first request for performance.

## OWL Ontology Support

QLever's GraphQL endpoint includes enhanced support for OWL/RDFS ontologies through optional materialized views that pre-compute inference results.

### How OWL Concepts Map to GraphQL

| OWL Concept | GraphQL Equivalent |
|-------------|-------------------|
| `owl:Class` / `rdfs:Class` | GraphQL Type |
| `owl:ObjectProperty` | Field returning another Type |
| `owl:DatatypeProperty` | Field returning Scalar |
| `rdfs:domain` | Which Type has this field |
| `rdfs:range` | Field's return type |
| `rdfs:subClassOf` | Type inheritance (with inference views) |

### OWL Inference Views

For large ontologies with class hierarchies, QLever can create materialized views that pre-compute inference results. This provides:

- **rdfs:subClassOf inference**: Instances inherit types from superclasses
- **rdfs:subPropertyOf inference**: Properties inherit from parent properties
- **owl:inverseOf expansion**: Bidirectional relationships
- **owl:sameAs unification**: Entity resolution
- **Property inheritance**: Types inherit properties from superclasses

### Creating OWL Inference Views

OWL inference views can be created automatically or manually:

**Automatic generation (in configuration):**
```cpp
SchemaBuilderConfig config;
config.useOwlInference = true;
config.autoGenerateOwlViews = true;
```

**Manual generation via HTTP:**
```bash
# Analyze ontology and get recommended views
curl "http://localhost:7001/graphql?action=analyze-ontology"

# Create a specific view (example: subClassOf closure)
POST /?cmd=write-materialized-view&view-name=owl-inferred-types
SELECT ?instance ?class ?depth WHERE {
  {
    ?instance a ?class .
    BIND(0 AS ?depth)
  }
  UNION
  {
    ?instance a ?directClass .
    ?directClass rdfs:subClassOf ?class .
    BIND(1 AS ?depth)
  }
}
```

### Available OWL Inference Views

| View Name | Purpose | When to Use |
|-----------|---------|-------------|
| `owl-inferred-types` | Type inference via subClassOf | Class hierarchies with 10+ subClassOf relationships |
| `owl-inferred-properties` | Property inference via subPropertyOf | Property hierarchies |
| `owl-inverse-properties` | owl:inverseOf expansion | Bidirectional relationships |
| `owl-unified-entities` | owl:sameAs resolution | Multiple identifiers for same entity |
| `owl-interface-membership` | GraphQL interface detection | Multiple inheritance patterns |
| `owl-instance-properties` | Property applicability per type | Domain-based property inheritance |

### GraphQL Interfaces from OWL

When OWL inference views are enabled, classes that are superclasses of multiple other classes are detected as potential GraphQL interfaces:

**OWL Ontology:**
```turtle
ex:Person a owl:Class .
ex:Organization a owl:Class .
ex:Agent a owl:Class .
ex:Person rdfs:subClassOf ex:Agent .
ex:Organization rdfs:subClassOf ex:Agent .
```

**Generated GraphQL (conceptual):**
```graphql
interface Agent {
  name: String
}

type Person implements Agent {
  name: String
  birthDate: Date
}

type Organization implements Agent {
  name: String
  foundingDate: Date
}
```

### OWL Limitations

The following OWL constructs have limited support:

| OWL Feature | Support Status |
|-------------|---------------|
| `owl:Class`, `rdfs:Class` | Fully supported |
| `owl:ObjectProperty`, `owl:DatatypeProperty` | Fully supported |
| `rdfs:domain`, `rdfs:range` | Fully supported |
| `rdfs:subClassOf` (transitive) | Supported via views |
| `rdfs:subPropertyOf` | Supported via views |
| `owl:inverseOf` | Supported via views |
| `owl:sameAs` | Supported via views |
| `owl:unionOf` | Partial (expanded in views) |
| `owl:intersectionOf` | Not supported |
| `owl:Restriction` | Not supported |
| `owl:cardinality` | Not supported |
| `owl:equivalentClass` | Not supported |

### Pre-materializing OWL Inferences

For best performance with OWL ontologies, pre-materialize inferences before indexing:

```bash
# Using Apache Jena's reasoner
riot --output=ntriples ontology.ttl data.ttl | \
  rdfinfer --rdfs ontology.ttl > materialized.nt

# Using ROBOT (OWL reasoner)
robot reason --input ontology.owl \
  --reasoner HermiT \
  --output materialized.owl

# Then index the materialized data
qlever-index -i my_index -f materialized.nt
```

## Comparison: GraphQL vs SPARQL

| Feature | GraphQL | SPARQL |
|---------|---------|--------|
| Learning curve | Lower for web developers | Requires RDF knowledge |
| Flexibility | Structured queries | Full query expressiveness |
| Aggregations | Not supported | Full support |
| Subqueries | Via nested fields | Full support |
| Updates | Experimental mutations | Via SPARQL Update |

Use GraphQL when:
- You want a simple API for web applications
- Your queries follow entity/relationship patterns
- You don't need complex aggregations

Use SPARQL when:
- You need complex joins or unions
- You need aggregations (COUNT, SUM, etc.)
- You need full RDF query expressiveness

## Examples

### Wikidata Scientists

```graphql
query {
  Scientist(filter: { name: { contains: "Einstein" } }, first: 10) {
    id
    name
    birthDate
    birthPlace {
      id
      name
    }
    knownFor {
      id
      name
    }
  }
}
```

### Organizations with Employees

```graphql
query {
  Organization(first: 5) {
    id
    name
    employees(first: 10) {
      id
      name
      jobTitle
    }
  }
}
```

## Troubleshooting

### "Unknown type" error
The type may not exist in your RDF data. Use introspection to see available types:
```graphql
{ __schema { types { name } } }
```

### "Query depth limit exceeded"
Your query has too many nested levels. Simplify the query or increase the depth limit.

### Empty results
- Check that your filter values are correct
- Verify the field names match your RDF properties
- Use introspection to see available fields

### Slow queries
- Add `first` limits to nested fields
- Avoid deeply nested queries
- Consider using SPARQL for complex queries

## Further Reading

- [GraphQL Specification](https://spec.graphql.org/)
- [GraphQL over HTTP](https://graphql.github.io/graphql-over-http/)
- [QLever SPARQL Documentation](quickstart.md)
