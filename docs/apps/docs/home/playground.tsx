"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import { GraphView, type GraphNode, type GraphEdge } from "./graph-view";

interface Dataset {
  label: string;
  dbFile: string;
  walFile: string;
  exampleQueries: { label: string; query: string }[];
  initialQuery: string;
  schema: string;
}

const DATASETS: Dataset[] = [
  {
    label: "Game of Thrones",
    dbFile: "/data/got.db",
    walFile: "/data/got.db-wal",
    exampleQueries: [
      { label: "Full graph", query: "MATCH (a)-[r]->(b) RETURN a, r, b" },
      { label: "All characters", query: "MATCH (n:Character) RETURN n.name ORDER BY n.name" },
      { label: "Jon's network", query: "MATCH (jon:Character {id: 'Jon'})-[r:INTERACTS_WITH]-(other) RETURN jon, r, other" },
      { label: "Strongest bonds", query: "MATCH (a)-[r:INTERACTS_WITH]->(b) WHERE r.weight >= 10 RETURN a, r, b" },
      { label: "Most connected", query: "MATCH (n:Character)-[r:INTERACTS_WITH]-() RETURN n.name, count(r) AS connections ORDER BY connections DESC LIMIT 15" },
      { label: "Lannister circle", query: "MATCH (l:Character)-[r:INTERACTS_WITH]-(other) WHERE l.id IN ['Tyrion', 'Cersei', 'Jaime', 'Tywin'] RETURN l, r, other" },
    ],
    initialQuery: "MATCH (a)-[r]->(b) RETURN a, r, b",
    schema: "(:Character { id, name }) -[:INTERACTS_WITH { weight }]- (:Character)",
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
    schema: "(:Hero { id, name }) -[:ALLIES_WITH { weight }]- (:Hero)",
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

export function CypherPlayground({ isEn }: { isEn: boolean }) {
  const [datasetIdx, setDatasetIdx] = useState(0);
  const [query, setQuery] = useState(DATASETS[0].initialQuery);
  const [result, setResult] = useState<QueryResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [duration, setDuration] = useState<number | null>(null);
  const [status, setStatus] = useState<Status>("loading");
  const [statusMsg, setStatusMsg] = useState("");
  const [viewMode, setViewMode] = useState<ViewMode>("graph");

  const moduleRef = useRef<any>(null);
  const dbRef = useRef<number>(0);
  const txnRef = useRef<number>(0);
  const loadingRef = useRef(false);

  const dataset = DATASETS[datasetIdx];

  // Load WASM module + fetch DB file
  const loadDatabase = useCallback(async (dsIdx: number) => {
    if (loadingRef.current) return;
    loadingRef.current = true;
    setStatus("loading");
    setResult(null);
    setError(null);
    setDuration(null);

    const ds = DATASETS[dsIdx];

    try {
      // Load WASM module if not loaded
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

      // Close previous DB if open
      if (dbRef.current) {
        if (txnRef.current) {
          mod.ccall("zyx_txn_close", null, ["number"], [txnRef.current]);
          txnRef.current = 0;
        }
        mod.ccall("zyx_close", null, ["number"], [dbRef.current]);
        dbRef.current = 0;
      }

      // Fetch pre-built database file
      setStatusMsg(isEn ? `Loading ${ds.label}...` : `加载 ${ds.label}...`);
      const dbPath = `/playground_${dsIdx}.db`;
      const walPath = `${dbPath}-wal`;

      // Clean previous MEMFS files
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

      // Open database
      const db = mod.ccall("zyx_open", "number", ["string"], [dbPath]);
      if (!db) throw new Error("Failed to open database");
      dbRef.current = db;

      // Open a read-only transaction to prevent any mutations
      const txn = mod.ccall("zyx_begin_read_only_transaction", "number", ["number"], [db]);
      if (!txn) throw new Error("Failed to begin read-only transaction");
      txnRef.current = txn;

      setStatus("ready");
      setStatusMsg("");
      loadingRef.current = false;

      // Auto-run initial query to show graph
      runQuery(mod, db, ds.initialQuery);
    } catch (e: any) {
      setStatus("error");
      setError(e.message || "Failed to load");
      setStatusMsg("");
      loadingRef.current = false;
    }
  }, [isEn]);

  // Initial load
  useEffect(() => {
    loadDatabase(0);
  }, []);

  // Execute a query and update state (uses read-only transaction)
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
          case 5: { // Node
            const props = mod.ccall("zyx_result_get_props_json", "string", ["number", "number"], [resultPtr, i]);
            row.push(props || "(node)");
            // ZYXNode: int64 id (8 bytes) + ptr label (4 bytes) in wasm32
            const nbuf = mod._malloc(16);
            if (mod.ccall("zyx_result_get_node", "boolean", ["number", "number", "number"], [resultPtr, i, nbuf])) {
              const nodeId = mod.HEAP32[nbuf >> 2];
              let label = String(nodeId);
              try {
                const p = JSON.parse(props || "{}");
                label = p.name || p.id || p.title || String(nodeId);
              } catch {}
              nodeMap.set(nodeId, { id: nodeId, label });
            }
            mod._free(nbuf);
            break;
          }
          case 6: { // Edge
            const props = mod.ccall("zyx_result_get_props_json", "string", ["number", "number"], [resultPtr, i]);
            row.push(props || "(edge)");
            // ZYXEdge: int64 id (8) + int64 src (8) + int64 tgt (8) + ptr type (4) = 28+ bytes
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
    <div className="flex h-full w-full min-h-0 flex-col gap-4 p-4 md:p-6">
      <header className="shrink-0 rounded-xl border border-[rgba(154,175,198,0.2)] bg-[#111821] px-4 py-3">
        <div className="flex flex-wrap items-center gap-3">
          <div className="flex h-8 w-8 items-center justify-center rounded-lg border border-[rgba(154,175,198,0.22)] bg-[#1a2430]">
            <span className="font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.68rem] font-bold text-[#b1c2d4]">
              {">_"}
            </span>
          </div>

          <div className="min-w-0">
            <h2 className="m-0 text-[1rem] font-semibold tracking-[0.02em] text-[#edf3f9]">
              {isEn ? "Cypher Playground" : "Cypher 试验场"}
            </h2>
            <p className="m-0 text-[0.73rem] text-[#8ea2b7]">
              {isEn ? "Interactive Graph Query Workspace" : "交互式图查询工作台"}
            </p>
          </div>

          <div className="ml-auto flex items-center gap-2">
            <div className={`flex items-center gap-1.5 rounded-full border px-2.5 py-1 text-[0.62rem] font-semibold ${
              status === "ready"
                ? "border-[rgba(74,222,128,0.3)] bg-[rgba(74,222,128,0.1)] text-[#4ade80]"
                : status === "loading"
                  ? "border-[rgba(250,204,21,0.3)] bg-[rgba(250,204,21,0.1)] text-[#facc15]"
                  : "border-[rgba(248,113,113,0.3)] bg-[rgba(248,113,113,0.1)] text-[#f87171]"
            }`}>
              <span className={`inline-block h-1.5 w-1.5 rounded-full ${
                status === "ready"
                  ? "bg-[#4ade80]"
                  : status === "loading"
                    ? "bg-[#facc15] animate-pulse"
                    : "bg-[#f87171]"
              }`} />
              {status === "ready"
                ? (isEn ? "Ready" : "就绪")
                : status === "loading"
                  ? (statusMsg || (isEn ? "Loading..." : "加载中..."))
                  : (isEn ? "Error" : "错误")}
            </div>

            {duration !== null && (
              <div className="rounded-md border border-[rgba(154,175,198,0.2)] bg-[#0f161f] px-2 py-1 font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.66rem] text-[#9eb2c6]">
                {duration.toFixed(2)} ms
              </div>
            )}
          </div>
        </div>
      </header>

      <div className="grid min-h-0 flex-1 grid-cols-1 gap-4 xl:grid-cols-[minmax(0,1fr)_360px]">
        <section className="flex min-h-0 flex-col overflow-hidden rounded-xl border border-[rgba(154,175,198,0.2)] bg-[#0f161f]">
          <div className="flex shrink-0 items-center gap-2 border-b border-[rgba(154,175,198,0.16)] px-3 py-2.5">
            <div className="min-w-0">
              <p className="m-0 truncate text-[0.73rem] font-semibold tracking-[0.05em] text-[#b1c2d4]">
                {dataset.label}
              </p>
              <p className="m-0 truncate text-[0.62rem] text-[#7f96ad]">
                {dataset.schema}
              </p>
            </div>

            {result && (
              <div className="ml-auto flex items-center overflow-hidden rounded-md border border-[rgba(154,175,198,0.2)] bg-[#131d28]">
                <button
                  onClick={() => setViewMode("graph")}
                  disabled={!hasGraphData}
                  className={`px-2.5 py-1 text-[0.62rem] font-medium transition-colors ${
                    viewMode === "graph"
                      ? "bg-[#243243] text-[#edf3f9]"
                      : "text-[#8ea2b7] hover:text-[#d8e3ef]"
                  } disabled:cursor-not-allowed disabled:opacity-30`}
                >
                  Graph
                </button>
                <button
                  onClick={() => setViewMode("table")}
                  className={`px-2.5 py-1 text-[0.62rem] font-medium transition-colors ${
                    viewMode === "table"
                      ? "bg-[#243243] text-[#edf3f9]"
                      : "text-[#8ea2b7] hover:text-[#d8e3ef]"
                  }`}
                >
                  Table
                </button>
              </div>
            )}
          </div>

          <div className="relative min-h-0 flex-1">
            {error ? (
              <div className="absolute inset-0 flex items-center justify-center p-4 text-[0.82rem] text-[#f87171] font-['Space_Mono','Ubuntu_Mono',monospace]">
                {error}
              </div>
            ) : result && viewMode === "graph" && hasGraphData ? (
              <GraphView nodes={result.graphNodes} edges={result.graphEdges} />
            ) : result && viewMode === "table" && result.values.length > 0 ? (
              <div className="absolute inset-0 overflow-auto [scrollbar-width:thin] [scrollbar-color:rgba(154,175,198,0.28)_transparent]">
                <table className="w-full border-collapse text-[0.74rem]">
                  <thead>
                    <tr className="sticky top-0 border-b border-[rgba(154,175,198,0.2)] bg-[#101925]">
                      {result.columns.map((col, ci) => (
                        <th key={ci} className="whitespace-nowrap px-3 py-2 text-left font-semibold text-[#b1c2d4]">
                          {col}
                        </th>
                      ))}
                    </tr>
                  </thead>
                  <tbody className="font-['Space_Mono','Ubuntu_Mono',monospace]">
                    {result.values.map((row, ri) => (
                      <tr key={ri} className="border-b border-[rgba(154,175,198,0.1)] hover:bg-[#141f2a]">
                        {row.map((val, ci) => (
                          <td key={ci} className="max-w-[280px] truncate px-3 py-1.5 text-[#d3dfeb]">
                            {val}
                          </td>
                        ))}
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            ) : result && result.values.length === 0 ? (
              <div className="absolute inset-0 flex items-center justify-center text-[0.82rem] text-[#7f96ad]">
                {isEn ? "Query executed (no rows)" : "查询完成（无返回行）"}
              </div>
            ) : (
              <div className="absolute inset-0 flex items-center justify-center text-[0.82rem] text-[#7f96ad]">
                {status === "loading"
                  ? (statusMsg || (isEn ? "Loading..." : "加载中..."))
                  : (isEn ? "Run a query to see results" : "执行查询查看结果")}
              </div>
            )}
          </div>
        </section>

        <aside className="flex min-h-0 flex-col gap-3 rounded-xl border border-[rgba(154,175,198,0.2)] bg-[#111821] p-3">
          <div>
            <p className="m-0 text-[0.7rem] font-semibold tracking-[0.04em] text-[#9fb2c6]">
              {isEn ? "Dataset" : "数据集"}
            </p>
            <div className="mt-2 flex flex-wrap gap-1.5">
              {DATASETS.map((ds, i) => (
                <button
                  key={ds.label}
                  onClick={() => handleDatasetSwitch(i)}
                  disabled={status === "loading"}
                  className={`rounded-md border px-2.5 py-1 text-[0.62rem] font-medium transition-colors ${
                    i === datasetIdx
                      ? "border-[rgba(177,194,212,0.5)] bg-[#253445] text-[#eef4fa]"
                      : "border-[rgba(154,175,198,0.22)] bg-[#17222e] text-[#8ea2b7] hover:text-[#d8e3ef]"
                  } disabled:cursor-not-allowed disabled:opacity-45`}
                >
                  {ds.label}
                </button>
              ))}
            </div>
          </div>

          <div className="flex min-h-0 flex-1 flex-col">
            <p className="m-0 mb-2 text-[0.7rem] font-semibold tracking-[0.04em] text-[#9fb2c6]">
              {isEn ? "Examples" : "示例查询"}
            </p>
            <div className="min-h-0 overflow-y-auto space-y-1.5 pr-1 [scrollbar-width:thin] [scrollbar-color:rgba(154,175,198,0.22)_transparent]">
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
                  className={`w-full rounded-md border px-2.5 py-1.5 text-left text-[0.66rem] font-medium transition-colors ${
                    query === eq.query
                      ? "border-[rgba(177,194,212,0.5)] bg-[#253445] text-[#eef4fa]"
                      : "border-[rgba(154,175,198,0.18)] bg-[#17222e] text-[#8ea2b7] hover:text-[#d8e3ef]"
                  }`}
                >
                  {eq.label}
                </button>
              ))}
            </div>
          </div>

          <div className="shrink-0 border-t border-[rgba(154,175,198,0.16)] pt-3">
            <p className="m-0 mb-2 text-[0.7rem] font-semibold tracking-[0.04em] text-[#9fb2c6]">
              {isEn ? "Query Editor" : "查询编辑器"}
            </p>
            <textarea
              value={query}
              onChange={(e) => setQuery(e.target.value)}
              onKeyDown={handleKeyDown}
              spellCheck={false}
              rows={5}
              className="w-full resize-none rounded-md border border-[rgba(154,175,198,0.2)] bg-[#0f161f] p-2.5 font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.73rem] leading-relaxed text-[#edf3f9] outline-none focus:border-[rgba(177,194,212,0.45)] placeholder:text-[#6e859c]"
              placeholder={isEn ? "Enter Cypher query..." : "输入 Cypher 查询..."}
            />
            <div className="mt-2 flex items-center justify-between gap-2">
              <span className="text-[0.58rem] text-[#6e859c]">
                {typeof navigator !== "undefined" && navigator.platform?.includes("Mac") ? "⌘" : "Ctrl"}+Enter
              </span>
              <button
                onClick={handleRun}
                disabled={status !== "ready"}
                className="rounded-md border border-[rgba(177,194,212,0.35)] bg-[#2b3a4c] px-3 py-1 text-[0.72rem] font-semibold text-[#edf3f9] transition-colors hover:bg-[#364b61] disabled:cursor-not-allowed disabled:opacity-40"
              >
                {isEn ? "Run Query" : "执行查询"}
              </button>
            </div>
          </div>
        </aside>
      </div>
    </div>
  );
}
