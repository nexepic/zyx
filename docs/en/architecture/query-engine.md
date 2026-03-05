# Query Engine

## Parser

ANTLR4-based Cypher parser with full language support.

## Planner

Converts parsed Cypher to logical plans with rule-based optimization.

## Executor

Executes physical plans with various operators:
- Scan operators: NodeScan, EdgeScan
- Modification: CreateNode, MergeNode, Delete
- Query: Filter, Sort, Project
- Special: VectorSearch, TrainVectorIndex
