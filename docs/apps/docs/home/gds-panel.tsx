"use client";

import { useCallback, useState } from "react";

interface DatasetSchema {
  nodes: { label: string; props: string[] }[];
  edges: { type: string }[];
}

interface GdsPanelProps {
  isEn: boolean;
  schema: DatasetSchema;
  onRunGds: (queries: string[], scope: { nodeLabel: string; edgeType: string }) => void;
  status: string;
}

type AlgorithmId =
  | "pageRank"
  | "wcc"
  | "betweenness"
  | "closeness"
  | "dijkstra";

interface AlgorithmDef {
  id: AlgorithmId;
  name: string;
  nameZh: string;
  description: string;
  descriptionZh: string;
  params: ParamDef[];
  buildQuery: (graphName: string, params: Record<string, string>) => string;
}

interface ParamDef {
  key: string;
  label: string;
  labelZh: string;
  placeholder: string;
  required?: boolean;
  type?: "number" | "text";
}

const PROJ_NAME = "__pg";

const ALGORITHMS: AlgorithmDef[] = [
  {
    id: "pageRank",
    name: "PageRank",
    nameZh: "PageRank",
    description: "Measures node influence based on link structure",
    descriptionZh: "基于链接结构衡量节点影响力",
    params: [
      { key: "maxIter", label: "Max iterations", labelZh: "最大迭代数", placeholder: "20" },
      { key: "damping", label: "Damping factor", labelZh: "阻尼系数", placeholder: "0.85" },
    ],
    buildQuery: (g, p) => {
      const parts = [`'${g}'`];
      if (p.maxIter) parts.push(p.maxIter);
      if (p.maxIter && p.damping) parts.push(p.damping);
      return `CALL gds.pageRank.stream(${parts.join(", ")}) YIELD nodeId, score RETURN nodeId, score ORDER BY score DESC`;
    },
  },
  {
    id: "wcc",
    name: "Connected Components",
    nameZh: "连通分量 (WCC)",
    description: "Finds groups of connected nodes",
    descriptionZh: "发现连通的节点分组",
    params: [],
    buildQuery: (g) =>
      `CALL gds.wcc.stream('${g}') YIELD nodeId, componentId RETURN componentId, collect(nodeId) AS nodes ORDER BY componentId`,
  },
  {
    id: "betweenness",
    name: "Betweenness Centrality",
    nameZh: "介数中心性",
    description: "Identifies bridge nodes between communities",
    descriptionZh: "识别社区间的桥接节点",
    params: [
      { key: "sampling", label: "Sampling size", labelZh: "采样大小", placeholder: "0 (all)" },
    ],
    buildQuery: (g, p) => {
      const parts = [`'${g}'`];
      if (p.sampling) parts.push(p.sampling);
      return `CALL gds.betweenness.stream(${parts.join(", ")}) YIELD nodeId, score RETURN nodeId, score ORDER BY score DESC`;
    },
  },
  {
    id: "closeness",
    name: "Closeness Centrality",
    nameZh: "紧密中心性",
    description: "Measures how close a node is to all others",
    descriptionZh: "衡量节点到其他节点的距离",
    params: [],
    buildQuery: (g) =>
      `CALL gds.closeness.stream('${g}') YIELD nodeId, score RETURN nodeId, score ORDER BY score DESC`,
  },
  {
    id: "dijkstra",
    name: "Shortest Path",
    nameZh: "最短路径",
    description: "Weighted shortest path between two nodes",
    descriptionZh: "两节点间的加权最短路径",
    params: [
      { key: "startId", label: "Start node ID", labelZh: "起始节点 ID", placeholder: "1", required: true, type: "number" },
      { key: "endId", label: "End node ID", labelZh: "终止节点 ID", placeholder: "10", required: true, type: "number" },
    ],
    buildQuery: (g, p) =>
      `CALL gds.shortestPath.dijkstra.stream('${g}', ${p.startId}, ${p.endId}) YIELD nodeId, cost RETURN nodeId, cost`,
  },
];

export function GdsPanel({ isEn, schema, onRunGds, status }: GdsPanelProps) {
  const [collapsed, setCollapsed] = useState(false);
  const [selectedAlgo, setSelectedAlgo] = useState<AlgorithmId>("pageRank");
  const [algoParams, setAlgoParams] = useState<Record<string, string>>({});
  const [running, setRunning] = useState(false);
  const [selectedNodeLabel, setSelectedNodeLabel] = useState("");
  const [selectedEdgeType, setSelectedEdgeType] = useState("");

  const algo = ALGORITHMS.find((a) => a.id === selectedAlgo)!;

  const setParam = useCallback((key: string, value: string) => {
    setAlgoParams((prev) => ({ ...prev, [key]: value }));
  }, []);

  const handleRun = useCallback(() => {
    if (status !== "ready") return;

    // Validate required params
    for (const p of algo.params) {
      if (p.required && !algoParams[p.key]) return;
    }

    // Build: drop old → project (with optional scope) → run algorithm
    const queries = [
      `CALL gds.graph.drop('${PROJ_NAME}')`,
      `CALL gds.graph.project('${PROJ_NAME}', '${selectedNodeLabel}', '${selectedEdgeType}')`,
      algo.buildQuery(PROJ_NAME, algoParams),
    ];
    setRunning(true);
    onRunGds(queries, { nodeLabel: selectedNodeLabel, edgeType: selectedEdgeType });
    setTimeout(() => setRunning(false), 50);
  }, [status, algo, algoParams, selectedNodeLabel, selectedEdgeType, onRunGds]);

  const scopeDesc = isEn
    ? (selectedNodeLabel || selectedEdgeType
        ? `Scope: ${selectedNodeLabel || "All nodes"} + ${selectedEdgeType || "All edges"}`
        : "Scope: Entire graph")
    : (selectedNodeLabel || selectedEdgeType
        ? `范围：${selectedNodeLabel || "全部节点"} + ${selectedEdgeType || "全部边"}`
        : "范围：全图");

  /* ── Collapsed state ── */
  if (collapsed) {
    return (
      <div className="flex w-[44px] shrink-0 flex-col items-center border-l border-[rgba(122,144,170,0.15)] bg-[rgba(11,15,20,0.5)]">
        <button
          onClick={() => setCollapsed(false)}
          className="mt-4 flex h-8 w-8 items-center justify-center rounded-md border border-[rgba(122,144,170,0.15)] bg-[rgba(122,144,170,0.06)] text-[#6b7f94] transition-colors hover:border-[rgba(122,144,170,0.3)] hover:text-[#8a9bb0]"
          title={isEn ? "Open Analytics" : "打开图分析"}
        >
          <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" strokeLinejoin="round">
            <polyline points="2,12 6,6 10,9 14,3" />
            <polyline points="11,3 14,3 14,6" />
          </svg>
        </button>
        <span className="mt-3 text-[0.55rem] font-medium uppercase tracking-[0.15em] text-[#4a5e72] [writing-mode:vertical-lr]">
          {isEn ? "Analytics" : "图分析"}
        </span>
      </div>
    );
  }

  const hasSchema = schema.nodes.length > 0 || schema.edges.length > 0;

  /* ── Expanded panel ── */
  return (
    <aside className="flex w-[300px] shrink-0 flex-col border-l border-[rgba(122,144,170,0.15)] bg-[rgba(11,15,20,0.5)] backdrop-blur-md">
      {/* Header */}
      <div className="flex h-[44px] shrink-0 items-center justify-between border-b border-[rgba(122,144,170,0.15)] px-4">
        <div className="flex items-center gap-2">
          <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="#6b8a9c" strokeWidth="1.4" strokeLinecap="round" strokeLinejoin="round">
            <polyline points="2,12 6,6 10,9 14,3" />
            <polyline points="11,3 14,3 14,6" />
          </svg>
          <span className="text-[0.7rem] font-medium uppercase tracking-widest text-[#8a9bb0]">
            {isEn ? "Analytics" : "图分析"}
          </span>
        </div>
        <button
          onClick={() => setCollapsed(true)}
          className="flex h-6 w-6 items-center justify-center rounded text-[#566b82] transition-colors hover:text-[#8a9bb0]"
          title={isEn ? "Collapse" : "收起"}
        >
          <svg width="12" height="12" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round">
            <path d="M8 2L4 6L8 10" />
          </svg>
        </button>
      </div>

      {/* Scrollable body */}
      <div className="min-h-0 flex-1 overflow-y-auto px-4 pb-4 [scrollbar-width:thin] [scrollbar-color:rgba(122,144,170,0.2)_transparent]">

        {/* Projection scope */}
        {hasSchema && (
          <>
            <p className="m-0 mt-4 mb-2.5 text-[0.6rem] font-medium uppercase tracking-[0.15em] text-[#566b82]">
              {isEn ? "Projection Scope" : "投影范围"}
            </p>

            {/* Node label selector */}
            <label className="mb-1 block text-[0.6rem] text-[#566b82]">
              {isEn ? "Node Label" : "节点标签"}
            </label>
            <div className="mb-3 flex flex-wrap gap-1">
              <button
                onClick={() => setSelectedNodeLabel("")}
                className={`rounded-full px-2 py-0.5 text-[0.65rem] font-medium transition-colors border ${
                  selectedNodeLabel === ""
                    ? "border-[rgba(107,138,118,0.5)] bg-[rgba(107,138,118,0.15)] text-[#8aad96]"
                    : "border-[rgba(122,144,170,0.15)] text-[#566b82] hover:text-[#8a9bb0] hover:border-[rgba(122,144,170,0.3)]"
                }`}
              >
                {isEn ? "All" : "全部"}
              </button>
              {schema.nodes.map((n) => (
                <button
                  key={n.label}
                  onClick={() => setSelectedNodeLabel(selectedNodeLabel === n.label ? "" : n.label)}
                  className={`rounded-full px-2 py-0.5 text-[0.65rem] font-medium transition-colors border ${
                    selectedNodeLabel === n.label
                      ? "border-[rgba(107,138,118,0.5)] bg-[rgba(107,138,118,0.15)] text-[#8aad96]"
                      : "border-[rgba(122,144,170,0.15)] text-[#566b82] hover:text-[#8a9bb0] hover:border-[rgba(122,144,170,0.3)]"
                  }`}
                >
                  {n.label}
                </button>
              ))}
            </div>

            {/* Edge type selector */}
            <label className="mb-1 block text-[0.6rem] text-[#566b82]">
              {isEn ? "Edge Type" : "边类型"}
            </label>
            <div className="mb-1 flex flex-wrap gap-1">
              <button
                onClick={() => setSelectedEdgeType("")}
                className={`rounded-full px-2 py-0.5 text-[0.65rem] font-medium transition-colors border ${
                  selectedEdgeType === ""
                    ? "border-[rgba(163,141,93,0.5)] bg-[rgba(163,141,93,0.12)] text-[#b8a473]"
                    : "border-[rgba(122,144,170,0.15)] text-[#566b82] hover:text-[#8a9bb0] hover:border-[rgba(122,144,170,0.3)]"
                }`}
              >
                {isEn ? "All" : "全部"}
              </button>
              {schema.edges.map((e) => (
                <button
                  key={e.type}
                  onClick={() => setSelectedEdgeType(selectedEdgeType === e.type ? "" : e.type)}
                  className={`rounded-full px-2 py-0.5 text-[0.65rem] font-medium font-['Space_Mono','Ubuntu_Mono',monospace] transition-colors border ${
                    selectedEdgeType === e.type
                      ? "border-[rgba(163,141,93,0.5)] bg-[rgba(163,141,93,0.12)] text-[#b8a473]"
                      : "border-[rgba(122,144,170,0.15)] text-[#566b82] hover:text-[#8a9bb0] hover:border-[rgba(122,144,170,0.3)]"
                  }`}
                >
                  {e.type}
                </button>
              ))}
            </div>

            <div className="my-4 border-t border-[rgba(122,144,170,0.08)]" />
          </>
        )}

        {/* Algorithm selector */}
        <p className={`m-0 mb-2.5 text-[0.6rem] font-medium uppercase tracking-[0.15em] text-[#566b82] ${!hasSchema ? "mt-4" : ""}`}>
          {isEn ? "Algorithm" : "算法"}
        </p>
        <div className="space-y-0.5">
          {ALGORITHMS.map((a) => {
            const active = selectedAlgo === a.id;
            return (
              <button
                key={a.id}
                onClick={() => { setSelectedAlgo(a.id); setAlgoParams({}); }}
                className={`w-full rounded-md px-3 text-left transition-all border ${
                  active
                    ? "border-[rgba(107,138,156,0.3)] bg-[rgba(107,138,156,0.08)] py-2.5"
                    : "border-transparent py-2 text-[#8a9bb0] hover:bg-[rgba(122,144,170,0.04)] hover:text-[#aec0d2]"
                }`}
              >
                <span className={`text-[0.75rem] font-medium ${active ? "text-[#e7edf5]" : ""}`}>
                  {isEn ? a.name : a.nameZh}
                </span>
                {active && (
                  <p className="m-0 mt-1 text-[0.65rem] leading-snug text-[#6b7f94]">
                    {isEn ? a.description : a.descriptionZh}
                  </p>
                )}
              </button>
            );
          })}
        </div>

        {/* Parameters (if any) */}
        {algo.params.length > 0 && (
          <>
            <div className="my-4 border-t border-[rgba(122,144,170,0.08)]" />
            <p className="m-0 mb-2.5 text-[0.6rem] font-medium uppercase tracking-[0.15em] text-[#566b82]">
              {isEn ? "Parameters" : "参数"}
            </p>
            <div className="space-y-3">
              {algo.params.map((p) => (
                <div key={p.key}>
                  <label className="mb-1 block text-[0.6rem] text-[#566b82]">
                    {isEn ? p.label : p.labelZh}
                    {p.required && <span className="ml-0.5 text-[#a37070]">*</span>}
                  </label>
                  <input
                    type="text"
                    inputMode={p.type === "number" ? "numeric" : undefined}
                    value={algoParams[p.key] || ""}
                    onChange={(e) => setParam(p.key, e.target.value)}
                    className="w-full rounded-md border border-[rgba(122,144,170,0.2)] bg-[rgba(11,15,20,0.6)] px-2.5 py-1.5 font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.75rem] text-[#e7edf5] outline-none transition-colors focus:border-[rgba(148,168,190,0.4)] placeholder:text-[#3a4d60]"
                    placeholder={p.placeholder}
                  />
                </div>
              ))}
            </div>
          </>
        )}
      </div>

      {/* Run button (pinned to bottom) */}
      <div className="shrink-0 border-t border-[rgba(122,144,170,0.1)] px-4 py-4">
        <button
          onClick={handleRun}
          disabled={status !== "ready" || running}
          className="w-full rounded-md border border-[rgba(107,138,156,0.35)] bg-[rgba(107,138,156,0.12)] px-3 py-2.5 text-[0.75rem] font-medium text-[#a0c4d8] transition-all hover:bg-[rgba(107,138,156,0.22)] hover:border-[rgba(107,138,156,0.5)] disabled:cursor-not-allowed disabled:opacity-40"
        >
          {running
            ? (isEn ? "Running..." : "运行中...")
            : isEn
              ? `Run ${algo.name}`
              : `运行${algo.nameZh}`}
        </button>
        <p className="m-0 mt-2 text-center text-[0.6rem] text-[#3a4d60]">
          {scopeDesc}
        </p>
      </div>
    </aside>
  );
}
