export function escapeCypherIdentifier(identifier: string): string {
  return `\`${identifier.replace(/`/g, '``')}\``;
}

export function escapeCypherStringLiteral(value: string): string {
  return `'${value.replace(/\\/g, '\\\\').replace(/'/g, "\\'")}'`;
}

export function buildScopedGraphMatchQuery(nodeLabel: string, edgeType: string): string {
  const filters: string[] = [];
  if (nodeLabel) {
    // The algorithm projection only keeps edges with both endpoints in scope.
    const label = escapeCypherStringLiteral(nodeLabel);
    filters.push(`${label} IN labels(a)`, `${label} IN labels(b)`);
  }
  if (edgeType) filters.push(`type(r) = ${escapeCypherStringLiteral(edgeType)}`);
  const where = filters.length ? ` WHERE ${filters.join(' AND ')}` : '';
  return `MATCH (a)-[r]->(b)${where} RETURN a, r, b`;
}
