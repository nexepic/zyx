export function escapeCypherIdentifier(identifier: string): string {
  return `\`${identifier.replace(/`/g, '``')}\``;
}

export function escapeCypherStringLiteral(value: string): string {
  return `'${value.replace(/\\/g, '\\\\').replace(/'/g, "\\'")}'`;
}

export function buildScopedGraphMatchQuery(nodeLabel: string, edgeType: string): string {
  const labelFilter = nodeLabel ? `:${escapeCypherIdentifier(nodeLabel)}` : '';
  const typeFilter = edgeType ? `:${escapeCypherIdentifier(edgeType)}` : '';
  return `MATCH (a${labelFilter})-[r${typeFilter}]->(b) RETURN a, r, b`;
}
