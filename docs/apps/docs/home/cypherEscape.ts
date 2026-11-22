export function escapeCypherIdentifier(identifier: string): string {
  return `\`${identifier.replace(/`/g, '``')}\``;
}

export function escapeCypherStringLiteral(value: string): string {
  return `'${value.replace(/\\/g, '\\\\').replace(/'/g, "\\'")}'`;
}

export function buildScopedGraphMatchQuery(nodeLabel: string, edgeType: string): string {
  const labelFilter = nodeLabel ? `:${escapeCypherIdentifier(nodeLabel)}` : '';
  const typeFilter = edgeType ? `:${escapeCypherIdentifier(edgeType)}` : '';
  // The algorithm projection only keeps edges with both endpoints in scope.
  return `MATCH (a${labelFilter})-[r${typeFilter}]->(b${labelFilter}) RETURN a, r, b`;
}
