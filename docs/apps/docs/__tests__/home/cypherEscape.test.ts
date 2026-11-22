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
      .toBe('MATCH (a:`Person`)-[r:`DIRECTED`]->(b:`Person`) RETURN a, r, b')
    expect(buildScopedGraphMatchQuery('Person', ''))
      .toBe('MATCH (a:`Person`)-[r]->(b:`Person`) RETURN a, r, b')
    expect(buildScopedGraphMatchQuery('', 'DIRECTED'))
      .toBe('MATCH (a)-[r:`DIRECTED`]->(b) RETURN a, r, b')
  })

  it('builds scoped graph queries without interpolating raw labels or types', () => {
    const query = buildScopedGraphMatchQuery('Label`) MATCH (evil) //', 'TYPE`) DELETE r //')

    expect(query).toBe('MATCH (a:`Label``) MATCH (evil) //`)-[r:`TYPE``) DELETE r //`]->(b:`Label``) MATCH (evil) //`) RETURN a, r, b')
    expect(query).not.toContain('(a:Label`)')
    expect(query).not.toContain('[r:TYPE`)')
  })
})
