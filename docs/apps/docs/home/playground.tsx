"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import Link from "next/link";
import { GraphView, type GraphNode, type GraphEdge } from "./graph-view";

interface SchemaNode {
  label: string;
  props: string[];
}

interface SchemaEdge {
  type: string;
}

interface DatasetSchema {
  nodes: SchemaNode[];
  edges: SchemaEdge[];
}

interface Dataset {
  label: string;
  dbFile: string;
  walFile: string;
  exampleQueries: { label: string; query: string }[];
  initialQuery: string;
}

const DATASETS: Dataset[] = [
  {
    label: "Game of Thrones",
    dbFile: "/data/got.db",
    walFile: "/data/got.db-wal",
    exampleQueries: [
      { label: "Full graph", query: "MATCH (a)-[r]->(b) RETURN a, r, b" },
      { label: "Stark family", query: "MATCH (c:Character)-[r:BELONGS_TO]->(h:House {name: 'Stark'}) RETURN c, r, h" },
      { label: "All houses", query: "MATCH (c:Character)-[:BELONGS_TO]->(h:House) RETURN h.name, count(c) AS members ORDER BY members DESC" },
      { label: "Top killers", query: "MATCH (killer:Character)-[:KILLED]->(victim) RETURN killer.name, count(victim) AS kills ORDER BY kills DESC LIMIT 15" },
      { label: "Royal family tree", query: "MATCH (p:Character)-[r:PARENT_OF]->(c:Character) WHERE p.royal = true OR c.royal = true RETURN p, r, c" },
      { label: "Marriages", query: "MATCH (a:Character)-[r:MARRIED_TO]->(b:Character) RETURN a, r, b" },
      { label: "Who serves whom", query: "MATCH (s:Character)-[r:SERVES]->(lord:Character) RETURN s, r, lord" },
    ],
    initialQuery: "MATCH (a)-[r]->(b) RETURN a, r, b",
  },
  {
    label: "Marvel Universe",
    dbFile: "/data/marvel.db",
    walFile: "/data/marvel.db-wal",
    exampleQueries: [
      { label: "Full graph", query: "MATCH (a)-[r]->(b) RETURN a, r, b" },
      { label: "All heroes", query: "MATCH (n:Hero) RETURN n.name ORDER BY n.name" },
      { label: "Spider-Man's network", query: "MATCH (s:Hero {id: 'SPIDER-MAN'})-[r:ALLIES_WITH]-(other) RETURN s, r, other" },
      { label: "Strongest alliances", query: "MATCH (a)-[r:ALLIES_WITH]->(b) WHERE r.weight >= 10 RETURN a, r, b" },
      { label: "Most connected", query: "MATCH (n:Hero)-[r:ALLIES_WITH]-() RETURN n.name, count(r) AS connections ORDER BY connections DESC LIMIT 15" },
      { label: "Avengers circle", query: "MATCH (a:Hero)-[r:ALLIES_WITH]-(other) WHERE a.id IN ['IRON-MAN', 'CAPTAIN-AMERICA', 'THOR', 'SPIDER-MAN'] RETURN a, r, other" },
    ],
    initialQuery: "MATCH (a)-[r]->(b) RETURN a, r, b",
  },
];

interface QueryResult {
  columns: string[];
  values: string[][];
  graphNodes: GraphNode[];
  graphEdges: GraphEdge[];
}

type Status = "loading" | "ready" | "error";
type ViewMode = "graph" | "table";

export function CypherPlayground({ isEn, homeLink }: { isEn: boolean; homeLink?: string }) {
  const [datasetIdx, setDatasetIdx] = useState(0);
  const [query, setQuery] = useState(DATASETS[0].initialQuery);
  const [result, setResult] = useState<QueryResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [duration, setDuration] = useState<number | null>(null);
  const [status, setStatus] = useState<Status>("loading");
  const [statusMsg, setStatusMsg] = useState("");
  const [viewMode, setViewMode] = useState<ViewMode>("graph");
  const [liveSchema, setLiveSchema] = useState<DatasetSchema>({ nodes: [], edges: [] });

  const moduleRef = useRef<any>(null);
  const dbRef = useRef<number>(0);
  const txnRef = useRef<number>(0);
  const loadingRef = useRef(false);

  const dataset = DATASETS[datasetIdx];

  const loadDatabase = useCallback(async (dsIdx: number) => {
    if (loadingRef.current) return;
    loadingRef.current = true;
    setStatus("loading");
    setError(null);
    setDuration(null);

    const ds = DATASETS[dsIdx];

    try {
      let mod = moduleRef.current;
      if (!mod) {
        setStatusMsg(isEn ? "Loading engine..." : "加载引擎...");
        await new Promise<void>((resolve, reject) => {
          const script = document.createElement("script");
          script.src = "/wasm/zyx.js";
          script.onload = () => resolve();
          script.onerror = () => reject(new Error("Failed to load /wasm/zyx.js"));
          document.head.appendChild(script);
        });

        const createModule = (window as any).createZyxModule;
        if (!createModule) throw new Error("WASM module factory not found");
        mod = await createModule();
        moduleRef.current = mod;
      }

      if (dbRef.current) {
        if (txnRef.current) {
          mod.ccall("zyx_txn_close", null, ["number"], [txnRef.current]);
          txnRef.current = 0;
        }
        mod.ccall("zyx_close", null, ["number"], [dbRef.current]);
        dbRef.current = 0;
      }

      setStatusMsg(isEn ? `Loading ${ds.label}...` : `加载 ${ds.label}...`);
      const dbPath = `/playground_${dsIdx}.db`;
      const walPath = `${dbPath}-wal`;

      try { mod.FS.unlink(dbPath); } catch {}
      try { mod.FS.unlink(walPath); } catch {}

      const [dbResp, walResp] = await Promise.all([
        fetch(ds.dbFile),
        fetch(ds.walFile).catch(() => null),
      ]);

      if (!dbResp.ok) throw new Error(`Failed to fetch ${ds.dbFile}: ${dbResp.status}`);

      const dbData = new Uint8Array(await dbResp.arrayBuffer());
      mod.FS.writeFile(dbPath, dbData);

      if (walResp && walResp.ok) {
        const walData = new Uint8Array(await walResp.arrayBuffer());
        mod.FS.writeFile(walPath, walData);
      }

      const db = mod.ccall("zyx_open", "number", ["string"], [dbPath]);
      if (!db) throw new Error("Failed to open database");
      dbRef.current = db;

      const txn = mod.ccall("zyx_begin_read_only_transaction", "number", ["number"], [db]);
      if (!txn) throw new Error("Failed to begin read-only transaction");
      txnRef.current = txn;

      setStatus("ready");
      setStatusMsg("");
      loadingRef.current = false;

      runQuery(mod, db, ds.initialQuery);
    } catch (e: any) {
      setStatus("error");
      setResult(null);
      setError(e.message || "Failed to load");
      setStatusMsg("");
      loadingRef.current = false;
    }
  }, [isEn]);

  useEffect(() => {
    loadDatabase(0);
  }, [loadDatabase]);

  const runQuery = useCallback((mod: any, db: number, cypher: string) => {
    const txn = txnRef.current;
    if (!txn) {
      setError("No active read-only transaction");
      return;
    }
    const resultPtr = mod.ccall("zyx_txn_execute", "number", ["number", "string"], [txn, cypher]);
    if (!resultPtr) {
      const errMsg = mod.ccall("zyx_get_last_error", "string", [], []);
      setError(errMsg || "Unknown error");
      return;
    }

    const success = mod.ccall("zyx_result_is_success", "boolean", ["number"], [resultPtr]);
    if (!success) {
      const errMsg = mod.ccall("zyx_result_get_error", "string", ["number"], [resultPtr]);
      mod.ccall("zyx_result_close", null, ["number"], [resultPtr]);
      setError(errMsg || "Query failed");
      return;
    }

    const dur = mod.ccall("zyx_result_get_duration", "number", ["number"], [resultPtr]);
    const colCount = mod.ccall("zyx_result_column_count", "number", ["number"], [resultPtr]);

    const columns: string[] = [];
    for (let i = 0; i < colCount; i++) {
      const name = mod.ccall("zyx_result_column_name", "string", ["number", "number"], [resultPtr, i]);
      columns.push(name || `col${i}`);
    }

    const values: string[][] = [];
    const nodeMap = new Map<number, GraphNode>();
    const edgeList: GraphEdge[] = [];
    const edgeSeen = new Set<number>();
    const schemaLabelProps = new Map<string, Set<string>>();
    const schemaEdgeTypes = new Set<string>();

    while (mod.ccall("zyx_result_next", "boolean", ["number"], [resultPtr])) {
      const row: string[] = [];
      for (let i = 0; i < colCount; i++) {
        const type = mod.ccall("zyx_result_get_type", "number", ["number", "number"], [resultPtr, i]);
        switch (type) {
          case 0: row.push("null"); break;
          case 1: row.push(mod.ccall("zyx_result_get_bool", "boolean", ["number", "number"], [resultPtr, i]) ? "true" : "false"); break;
          case 2: row.push(String(mod.ccall("zyx_result_get_int", "number", ["number", "number"], [resultPtr, i]))); break;
          case 3: row.push(String(mod.ccall("zyx_result_get_double", "number", ["number", "number"], [resultPtr, i]))); break;
          case 4: {
            const s = mod.ccall("zyx_result_get_string", "string", ["number", "number"], [resultPtr, i]);
            row.push(s || "");
            break;
          }
          case 5: {
            const props = mod.ccall("zyx_result_get_props_json", "string", ["number", "number"], [resultPtr, i]);
            row.push(props || "(node)");
            const nbuf = mod._malloc(16);
            if (mod.ccall("zyx_result_get_node", "boolean", ["number", "number", "number"], [resultPtr, i, nbuf])) {
              const nodeId = mod.HEAP32[nbuf >> 2];
              // Read node label from ZYXNode struct (label pointer at offset 8)
              const labelPtr = mod.HEAP32[(nbuf + 8) >> 2] >>> 0;
              const nodeLabel = labelPtr ? mod.UTF8ToString(labelPtr) : "";
              let displayLabel = String(nodeId);
              try {
                const p = JSON.parse(props || "{}");
                displayLabel = p.name || p.id || p.title || String(nodeId);
                // Collect schema: label → property keys
                if (nodeLabel) {
                  if (!schemaLabelProps.has(nodeLabel)) schemaLabelProps.set(nodeLabel, new Set());
                  for (const key of Object.keys(p)) {
                    schemaLabelProps.get(nodeLabel)!.add(key);
                  }
                }
              } catch {}
              nodeMap.set(nodeId, { id: nodeId, label: displayLabel });
            }
            mod._free(nbuf);
            break;
          }
          case 6: {
            const props = mod.ccall("zyx_result_get_props_json", "string", ["number", "number"], [resultPtr, i]);
            row.push(props || "(edge)");
            const ebuf = mod._malloc(32);
            if (mod.ccall("zyx_result_get_edge", "boolean", ["number", "number", "number"], [resultPtr, i, ebuf])) {
              const edgeId = mod.HEAP32[ebuf >> 2];
              const srcId = mod.HEAP32[(ebuf + 8) >> 2];
              const tgtId = mod.HEAP32[(ebuf + 16) >> 2];
              const typePtr = mod.HEAP32[(ebuf + 24) >> 2] >>> 0;
              const edgeType = typePtr ? mod.UTF8ToString(typePtr) : "";
              if (srcId && tgtId && !edgeSeen.has(edgeId)) {
                edgeSeen.add(edgeId);
                edgeList.push({ id: edgeId, sourceId: srcId, targetId: tgtId, type: edgeType });
                if (edgeType) schemaEdgeTypes.add(edgeType);
                if (!nodeMap.has(srcId)) nodeMap.set(srcId, { id: srcId, label: String(srcId) });
                if (!nodeMap.has(tgtId)) nodeMap.set(tgtId, { id: tgtId, label: String(tgtId) });
              }
            }
            mod._free(ebuf);
            break;
          }
          case 7: row.push("[list]"); break;
          case 8: {
            const mapJson = mod.ccall("zyx_result_get_map_json", "string", ["number", "number"], [resultPtr, i]);
            row.push(mapJson || "{}");
            break;
          }
          default: row.push("?");
        }
      }
      values.push(row);
    }

    mod.ccall("zyx_result_close", null, ["number"], [resultPtr]);

    // Build live schema from result
    const schemaNodes: SchemaNode[] = [];
    for (const [label, props] of schemaLabelProps) {
      schemaNodes.push({ label, props: Array.from(props).sort() });
    }
    schemaNodes.sort((a, b) => a.label.localeCompare(b.label));
    const schemaEdges: SchemaEdge[] = Array.from(schemaEdgeTypes).sort().map((t) => ({ type: t }));
    setLiveSchema({ nodes: schemaNodes, edges: schemaEdges });

    const graphNodes = Array.from(nodeMap.values());
    const hasGraph = graphNodes.length > 0 && edgeList.length > 0;

    setError(null);
    setResult({ columns, values, graphNodes, graphEdges: edgeList });
    setDuration(dur);
    setViewMode(hasGraph ? "graph" : "table");
  }, []);

  const handleRun = useCallback(() => {
    if (status !== "ready") return;
    const mod = moduleRef.current;
    const db = dbRef.current;
    if (!mod || !db) return;
    setError(null);
    runQuery(mod, db, query);
  }, [status, query, runQuery]);

  const handleKeyDown = useCallback((e: React.KeyboardEvent) => {
    if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
      e.preventDefault();
      handleRun();
    }
  }, [handleRun]);

  const handleDatasetSwitch = useCallback((idx: number) => {
    if (idx === datasetIdx || status === "loading") return;
    setDatasetIdx(idx);
    setQuery(DATASETS[idx].initialQuery);
    loadDatabase(idx);
  }, [datasetIdx, status, loadDatabase]);

  const hasGraphData = result ? result.graphNodes.length > 0 && result.graphEdges.length > 0 : false;

  return (
    <div className="flex h-full w-full min-h-0 flex-col bg-transparent text-[#e7edf5]">
      {/* Top bar - Minimalist borders */}
      <header className="shrink-0 flex items-center border-b border-[rgba(122,144,170,0.15)] px-6 py-3 bg-[rgba(11,15,20,0.4)] backdrop-blur-sm">
        {/* Left: logo + dataset switcher — never shrink */}
        <div className="flex shrink-0 items-center gap-5">
          <div className="flex items-center gap-3">
            {homeLink ? (
              <Link href={homeLink} className="font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.8rem] font-bold tracking-widest text-[#94a8be] no-underline transition-colors hover:text-[#e7edf5]">
                ZYX
              </Link>
            ) : (
              <span className="font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.8rem] font-bold tracking-widest text-[#94a8be]">
                ZYX
              </span>
            )}
            <span className="text-[rgba(122,144,170,0.3)]">|</span>
            <span className="text-[0.85rem] font-medium text-[#e7edf5]">
              {isEn ? "Playground" : "试验场"}
            </span>
          </div>

          <div className="flex items-center gap-1 rounded-md border border-[rgba(122,144,170,0.15)] bg-[rgba(11,15,20,0.5)] p-[3px]">
            {DATASETS.map((ds, i) => (
              <button
                key={ds.label}
                onClick={() => handleDatasetSwitch(i)}
                disabled={status === "loading"}
                className={`rounded px-3 py-1.5 text-[0.75rem] transition-colors ${
                  i === datasetIdx
                    ? "bg-[rgba(122,144,170,0.2)] text-[#e7edf5] font-medium shadow-sm"
                    : "text-[#8a9bb0] hover:text-[#e7edf5] hover:bg-[rgba(122,144,170,0.08)]"
                } disabled:cursor-not-allowed disabled:opacity-40`}
              >
                {ds.label}
              </button>
            ))}
          </div>
        </div>

        {/* Spacer — fills remaining space */}
        <div className="flex-1" />

        {/* Right: duration + status — never shrink */}
        <div className="flex shrink-0 items-center gap-4">
          {duration !== null && (
            <span className="font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.7rem] text-[#8a9bb0]">
              {duration.toFixed(2)} ms
            </span>
          )}
          <div className="flex items-center gap-2 text-[0.7rem] uppercase tracking-wider font-medium whitespace-nowrap">
            <span className={`inline-block h-2 w-2 shrink-0 rounded-full ${
              status === "ready"
                ? "bg-[#6b8a76]"
                : status === "loading"
                  ? "bg-[#a38d5d]"
                  : "bg-[#a35d5d]"
            }`} />
            <span className={
              status === "ready" ? "text-[#8a9bb0]" :
              status === "loading" ? "text-[#a38d5d]" : "text-[#a35d5d]"
            }>
              {status === "ready"
                ? (isEn ? "Ready" : "就绪")
                : status === "loading"
                  ? (statusMsg || (isEn ? "Loading..." : "加载中..."))
                  : (isEn ? "Error" : "错误")}
            </span>
          </div>
        </div>
      </header>

      {/* Main content */}
      <div className="flex min-h-0 flex-1">
        {/* Results panel (Left) */}
        <section className="flex min-h-0 flex-1 flex-col bg-[rgba(11,15,20,0.2)]">
          {/* Results toolbar */}
          <div className="flex h-[44px] shrink-0 items-center justify-between border-b border-[rgba(122,144,170,0.15)] px-6">
            <span className="text-[0.7rem] font-medium uppercase tracking-widest text-[#8a9bb0]">
              {isEn ? "Output" : "输出结果"}
            </span>

            {result && (
              <div className="flex items-center rounded-md border border-[rgba(122,144,170,0.15)] bg-[rgba(11,15,20,0.5)] p-[2px]">
                <button
                  onClick={() => setViewMode("graph")}
                  disabled={!hasGraphData}
                  className={`px-3 py-1 text-[0.7rem] transition-colors rounded-sm ${
                    viewMode === "graph"
                      ? "bg-[rgba(122,144,170,0.2)] text-[#e7edf5]"
                      : "text-[#8a9bb0] hover:text-[#e7edf5]"
                  } disabled:cursor-not-allowed disabled:opacity-30`}
                >
                  Graph
                </button>
                <button
                  onClick={() => setViewMode("table")}
                  className={`px-3 py-1 text-[0.7rem] transition-colors rounded-sm ${
                    viewMode === "table"
                      ? "bg-[rgba(122,144,170,0.2)] text-[#e7edf5]"
                      : "text-[#8a9bb0] hover:text-[#e7edf5]"
                  }`}
                >
                  Table
                </button>
              </div>
            )}
          </div>

          {/* Results area */}
          <div className="relative min-h-0 flex-1">
            {error ? (
              <div className="absolute inset-0 flex items-center justify-center p-6">
                <div className="max-w-lg rounded-md border border-[rgba(163,93,93,0.3)] bg-[rgba(163,93,93,0.1)] px-5 py-4 font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.8rem] text-[#c98a8a] leading-relaxed">
                  {error}
                </div>
              </div>
            ) : result && viewMode === "graph" && hasGraphData ? (
              <GraphView nodes={result.graphNodes} edges={result.graphEdges} />
            ) : result && viewMode === "table" && result.values.length > 0 ? (
              <div className="absolute inset-0 overflow-auto [scrollbar-width:thin] [scrollbar-color:rgba(122,144,170,0.2)_transparent]">
                <table className="w-full border-collapse text-[0.8rem]">
                  <thead>
                    <tr className="sticky top-0 border-b border-[rgba(122,144,170,0.15)] bg-[rgba(11,15,20,0.95)] backdrop-blur-sm z-10">
                      {result.columns.map((col, ci) => (
                        <th key={ci} className="whitespace-nowrap px-6 py-3 text-left font-medium text-[#9db0c4]">
                          {col}
                        </th>
                      ))}
                    </tr>
                  </thead>
                  <tbody className="font-['Space_Mono','Ubuntu_Mono',monospace]">
                    {result.values.map((row, ri) => (
                      <tr key={ri} className="border-b border-[rgba(122,144,170,0.05)] transition-colors hover:bg-[rgba(122,144,170,0.05)]">
                        {row.map((val, ci) => (
                          <td key={ci} className="max-w-[300px] truncate px-6 py-3 text-[#aec0d2]">
                            {val}
                          </td>
                        ))}
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            ) : result && result.values.length === 0 ? (
              <div className="absolute inset-0 flex items-center justify-center text-[0.85rem] text-[#8a9bb0]">
                {isEn ? "Query executed successfully (0 rows)" : "查询成功完成（无返回行）"}
              </div>
            ) : (
              <div className="absolute inset-0 flex flex-col items-center justify-center text-[0.85rem] text-[#566b82]">
                {status === "loading"
                  ? (statusMsg || (isEn ? "Loading Engine..." : "加载引擎中..."))
                  : (isEn ? "Run a Cypher query to see results" : "执行 Cypher 查询查看结果")}
              </div>
            )}
          </div>
        </section>

        {/* Sidebar (Right) */}
        <aside className="flex w-[380px] shrink-0 flex-col border-l border-[rgba(122,144,170,0.15)] bg-[rgba(11,15,20,0.6)] backdrop-blur-md">
          {/* Example queries */}
          <div className="flex min-h-0 flex-1 flex-col border-b border-[rgba(122,144,170,0.15)]">
            <div className="shrink-0 flex h-[44px] items-center justify-between px-6">
              <p className="m-0 text-[0.7rem] font-medium uppercase tracking-widest text-[#8a9bb0]">
                {isEn ? "Examples" : "示例查询"}
              </p>
              {/* Schema popover trigger */}
              {(liveSchema.nodes.length > 0 || liveSchema.edges.length > 0) && (
                <div className="group relative">
                  <button className="flex items-center gap-1.5 rounded-md border border-[rgba(122,144,170,0.15)] bg-[rgba(122,144,170,0.06)] px-2.5 py-1 text-[0.65rem] text-[#6b7f94] transition-colors hover:border-[rgba(122,144,170,0.3)] hover:text-[#8a9bb0]">
                    <svg width="12" height="12" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
                      <circle cx="4" cy="4" r="2.5" /><circle cx="12" cy="4" r="2.5" /><circle cx="8" cy="12" r="2.5" />
                      <line x1="6" y1="5" x2="7" y2="10" /><line x1="10" y1="5" x2="9" y2="10" />
                    </svg>
                    <span>Schema</span>
                    <span className="text-[#566b82]">
                      {liveSchema.nodes.length + liveSchema.edges.length}
                    </span>
                  </button>
                  {/* Popover */}
                  <div className="pointer-events-none absolute right-0 top-full z-30 mt-2 w-[280px] rounded-lg border border-[rgba(122,144,170,0.2)] bg-[#131820] p-4 opacity-0 shadow-xl transition-opacity group-hover:pointer-events-auto group-hover:opacity-100">
                    {liveSchema.nodes.length > 0 && (
                      <div>
                        <p className="m-0 mb-2 text-[0.6rem] font-medium uppercase tracking-widest text-[#566b82]">
                          {isEn ? "Nodes" : "节点"}
                        </p>
                        <div className="flex flex-wrap gap-1.5">
                          {liveSchema.nodes.map((n) => (
                            <span
                              key={n.label}
                              title={n.props.length > 0 ? n.props.join(", ") : undefined}
                              className="inline-flex items-center gap-1 rounded-full border border-[rgba(107,138,118,0.35)] bg-[rgba(107,138,118,0.1)] px-2 py-0.5 text-[0.65rem] font-medium text-[#8aad96]"
                            >
                              {n.label}
                              {n.props.length > 0 && (
                                <span className="text-[0.6rem] text-[#5a7a66]">{n.props.length}</span>
                              )}
                            </span>
                          ))}
                        </div>
                      </div>
                    )}
                    {liveSchema.edges.length > 0 && (
                      <div className={liveSchema.nodes.length > 0 ? "mt-3" : ""}>
                        <p className="m-0 mb-2 text-[0.6rem] font-medium uppercase tracking-widest text-[#566b82]">
                          {isEn ? "Relationships" : "关系"}
                        </p>
                        <div className="flex flex-wrap gap-1.5">
                          {liveSchema.edges.map((e) => (
                            <span
                              key={e.type}
                              className="inline-flex items-center rounded-full border border-[rgba(163,141,93,0.3)] bg-[rgba(163,141,93,0.08)] px-2 py-0.5 text-[0.6rem] font-medium text-[#b8a473] font-['Space_Mono','Ubuntu_Mono',monospace]"
                            >
                              {e.type}
                            </span>
                          ))}
                        </div>
                      </div>
                    )}
                  </div>
                </div>
              )}
            </div>
            <div className="min-h-0 flex-1 overflow-y-auto px-4 pb-4 [scrollbar-width:thin] [scrollbar-color:rgba(122,144,170,0.2)_transparent]">
              <div className="space-y-1">
                {dataset.exampleQueries.map((eq) => (
                  <button
                    key={eq.label}
                    onClick={() => {
                      setQuery(eq.query);
                      if (status === "ready" && moduleRef.current && dbRef.current) {
                        setError(null);
                        runQuery(moduleRef.current, dbRef.current, eq.query);
                      }
                    }}
                    className={`w-full rounded-md px-3 py-2.5 text-left text-[0.8rem] transition-colors border ${
                      query === eq.query
                        ? "bg-[rgba(122,144,170,0.1)] border-[rgba(122,144,170,0.2)] text-[#e7edf5]"
                        : "border-transparent text-[#8a9bb0] hover:bg-[rgba(122,144,170,0.05)] hover:text-[#aec0d2]"
                    }`}
                  >
                    {eq.label}
                  </button>
                ))}
              </div>
            </div>
          </div>

          {/* Query editor */}
          <div className="shrink-0 flex flex-col px-6 pt-5 pb-6">
            <div className="mb-3 flex items-center justify-between">
              <p className="m-0 text-[0.7rem] font-medium uppercase tracking-widest text-[#8a9bb0]">
                {isEn ? "Editor" : "编辑器"}
              </p>
              <span className="text-[0.65rem] text-[#566b82] font-['Space_Mono']">
                {typeof navigator !== "undefined" && navigator.platform?.includes("Mac") ? "⌘" : "Ctrl"} + Enter
              </span>
            </div>
            <textarea
              value={query}
              onChange={(e) => setQuery(e.target.value)}
              onKeyDown={handleKeyDown}
              spellCheck={false}
              rows={5}
              className="w-full resize-none rounded-md border border-[rgba(122,144,170,0.2)] bg-[rgba(11,15,20,0.6)] p-3 font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.8rem] leading-relaxed text-[#e7edf5] outline-none transition-colors focus:border-[rgba(148,168,190,0.5)] placeholder:text-[#566b82]"
              placeholder={isEn ? "MATCH (n) RETURN n LIMIT 10..." : "输入 Cypher 查询..."}
            />
            <button
              onClick={handleRun}
              disabled={status !== "ready"}
              className="mt-4 w-full rounded-md bg-[rgba(122,144,170,0.15)] border border-[rgba(122,144,170,0.2)] px-4 py-2.5 text-[0.8rem] text-[#e7edf5] transition-all hover:bg-[rgba(122,144,170,0.25)] hover:border-[rgba(122,144,170,0.4)] disabled:cursor-not-allowed disabled:opacity-40"
            >
              {isEn ? "Run Query" : "执行查询"}
            </button>
          </div>

          {/* Powered-by footer */}
          <div className="shrink-0 border-t border-[rgba(122,144,170,0.1)] px-6 py-3 flex items-center justify-center gap-1.5 text-[0.65rem] text-[#4a5e72]">
            <span>{isEn ? "Powered by" : "由"}</span>
            <span className="font-['Space_Mono','Ubuntu_Mono',monospace] font-semibold text-[#6b7f94]">ZYX</span>
            <span className="text-[#4a5e72]">WebAssembly</span>
            {!isEn && <span>驱动</span>}
          </div>
        </aside>
      </div>
    </div>
  );
}