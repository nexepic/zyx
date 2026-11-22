import { buildScopedGraphMatchQuery, escapeCypherIdentifier, escapeCypherStringLiteral } from '../../home/cypherEscape'

describe('Cypher playground escaping', () => {
  it('escapes schema-derived identifiers with backticks', () => {
    expect(escapeCypherIdentifier('Movie` MATCH (n) RETURN n //')).toBe('`Movie`` MATCH (n) RETURN n //`')
  })

  it('escapes GDS string literals with single quotes and backslashes', () => {
    expect(escapeCypherStringLiteral("proj'\\name")).toBe("'proj\\'\\\\name'")
  })

  it('matches only edges whose endpoints are in the projection node scope', () => {
    expect(buildScopedGraphMatchQuery('Person', 'DIRECTED'))
      .toBe("MATCH (a)-[r]->(b) WHERE 'Person' IN labels(a) AND 'Person' IN labels(b) AND type(r) = 'DIRECTED' RETURN a, r, b")
    expect(buildScopedGraphMatchQuery('Person', ''))
      .toBe("MATCH (a)-[r]->(b) WHERE 'Person' IN labels(a) AND 'Person' IN labels(b) RETURN a, r, b")
    expect(buildScopedGraphMatchQuery('', 'DIRECTED'))
      .toBe("MATCH (a)-[r]->(b) WHERE type(r) = 'DIRECTED' RETURN a, r, b")
    expect(buildScopedGraphMatchQuery('', '')).toBe('MATCH (a)-[r]->(b) RETURN a, r, b')
  })

  it('quotes labels and relationship types as string literals', () => {
    expect(buildScopedGraphMatchQuery("Director's Cut", "Related's Type"))
      .toBe("MATCH (a)-[r]->(b) WHERE 'Director\\'s Cut' IN labels(a) AND 'Director\\'s Cut' IN labels(b) AND type(r) = 'Related\\'s Type' RETURN a, r, b")
  })
})
