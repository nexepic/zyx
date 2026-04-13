"use client";

import { useCallback, useEffect, useRef, useState } from "react";

// Pre-built graph data: a small social + movie knowledge graph
const SEED_QUERIES = [
  `CREATE (alice:Person {name: 'Alice', age: 30, role: 'Engineer'})`,
  `CREATE (bob:Person {name: 'Bob', age: 28, role: 'Designer'})`,
  `CREATE (charlie:Person {name: 'Charlie', age: 35, role: 'Manager'})`,
  `CREATE (diana:Person {name: 'Diana', age: 26, role: 'Analyst'})`,
  `CREATE (eve:Person {name: 'Eve', age: 32, role: 'Researcher'})`,
  `CREATE (matrix:Movie {title: 'The Matrix', year: 1999, genre: 'Sci-Fi'})`,
  `CREATE (inception:Movie {title: 'Inception', year: 2010, genre: 'Sci-Fi'})`,
  `CREATE (interstellar:Movie {title: 'Interstellar', year: 2014, genre: 'Sci-Fi'})`,
  `MATCH (a:Person {name:'Alice'}), (b:Person {name:'Bob'}) CREATE (a)-[:KNOWS {since: 2019}]->(b)`,
  `MATCH (a:Person {name:'Alice'}), (c:Person {name:'Charlie'}) CREATE (a)-[:REPORTS_TO]->(c)`,
  `MATCH (b:Person {name:'Bob'}), (c:Person {name:'Charlie'}) CREATE (b)-[:REPORTS_TO]->(c)`,
  `MATCH (d:Person {name:'Diana'}), (a:Person {name:'Alice'}) CREATE (d)-[:KNOWS {since: 2021}]->(a)`,
  `MATCH (e:Person {name:'Eve'}), (d:Person {name:'Diana'}) CREATE (e)-[:KNOWS {since: 2020}]->(d)`,
  `MATCH (a:Person {name:'Alice'}), (m:Movie {title:'The Matrix'}) CREATE (a)-[:LIKES {rating: 5}]->(m)`,
  `MATCH (b:Person {name:'Bob'}), (m:Movie {title:'Inception'}) CREATE (b)-[:LIKES {rating: 4}]->(m)`,
  `MATCH (c:Person {name:'Charlie'}), (m:Movie {title:'Interstellar'}) CREATE (c)-[:LIKES {rating: 5}]->(m)`,
  `MATCH (d:Person {name:'Diana'}), (m:Movie {title:'The Matrix'}) CREATE (d)-[:LIKES {rating: 4}]->(m)`,
  `MATCH (e:Person {name:'Eve'}), (m:Movie {title:'Inception'}) CREATE (e)-[:LIKES {rating: 5}]->(m)`,
];

const EXAMPLE_QUERIES = [
  { label: "All nodes", query: "MATCH (n) RETURN n" },
  { label: "All relationships", query: "MATCH (a)-[r]->(b) RETURN a.name, type(r), b.name" },
  { label: "Friends of Alice", query: "MATCH (a:Person {name: 'Alice'})-[:KNOWS]->(friend) RETURN friend.name, friend.role" },
  { label: "Who likes Sci-Fi?", query: "MATCH (p:Person)-[:LIKES]->(m:Movie) RETURN p.name, m.title, m.genre" },
  { label: "Reporting chain", query: "MATCH (p:Person)-[:REPORTS_TO]->(mgr:Person) RETURN p.name AS employee, mgr.name AS manager" },
  { label: "Highly rated movies", query: "MATCH (p:Person)-[l:LIKES]->(m:Movie) WHERE l.rating >= 5 RETURN p.name, m.title, l.rating" },
];

interface ResultRow {
  columns: string[];
  values: string[][];
}

type ModuleStatus = "loading" | "ready" | "error";

export function CypherPlayground({ isEn }: { isEn: boolean }) {
  const [query, setQuery] = useState(EXAMPLE_QUERIES[0].query);
  const [result, setResult] = useState<ResultRow | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [duration, setDuration] = useState<number | null>(null);
  const [status, setStatus] = useState<ModuleStatus>("loading");
  const [seeded, setSeeded] = useState(false);

  const moduleRef = useRef<any>(null);
  const dbRef = useRef<number>(0);

  // Load WASM module
  useEffect(() => {
    let cancelled = false;

    async function init() {
      try {
        const script = document.createElement("script");
        script.src = "/wasm/zyx.js";
        script.onload = async () => {
          try {
            const createModule = (window as any).createZyxModule;
            if (!createModule) {
              setStatus("error");
              setError("Failed to load WASM module");
              return;
            }
            const mod = await createModule();
            if (cancelled) return;
            moduleRef.current = mod;

            // Open in-memory database using MEMFS
            const db = mod.ccall("zyx_open", "number", ["string"], ["/playground.db"]);

            if (!db) {
              setStatus("error");
              setError("Failed to open database");
              return;
            }

            dbRef.current = db;
            setStatus("ready");
          } catch (e: any) {
            if (!cancelled) {
              setStatus("error");
              setError(e.message || "WASM init failed");
            }
          }
        };
        script.onerror = () => {
          if (!cancelled) {
            setStatus("error");
            setError("Failed to load /wasm/zyx.js");
          }
        };
        document.head.appendChild(script);
      } catch (e: any) {
        if (!cancelled) {
          setStatus("error");
          setError(e.message || "Init failed");
        }
      }
    }

    init();
    return () => { cancelled = true; };
  }, []);

  // Seed data once module is ready
  useEffect(() => {
    if (status !== "ready" || seeded) return;

    const mod = moduleRef.current;
    const db = dbRef.current;
    if (!mod || !db) return;

    for (const seedQuery of SEED_QUERIES) {
      execQuery(mod, db, seedQuery);
    }
    setSeeded(true);
  }, [status, seeded]);

  const execQuery = useCallback((mod: any, db: number, cypher: string) => {
    const resultPtr = mod.ccall("zyx_execute", "number", ["number", "string"], [db, cypher]);

    if (!resultPtr) {
      const errMsg = mod.ccall("zyx_get_last_error", "string", [], []);
      return { error: errMsg || "Unknown error", columns: [], values: [], duration: 0 };
    }

    const success = mod.ccall("zyx_result_is_success", "boolean", ["number"], [resultPtr]);
    if (!success) {
      const errMsg = mod.ccall("zyx_result_get_error", "string", ["number"], [resultPtr]);
      mod.ccall("zyx_result_close", null, ["number"], [resultPtr]);
      return { error: errMsg || "Query failed", columns: [], values: [], duration: 0 };
    }

    const dur = mod.ccall("zyx_result_get_duration", "number", ["number"], [resultPtr]);
    const colCount = mod.ccall("zyx_result_column_count", "number", ["number"], [resultPtr]);

    const columns: string[] = [];
    for (let i = 0; i < colCount; i++) {
      const name = mod.ccall("zyx_result_column_name", "string", ["number", "number"], [resultPtr, i]);
      columns.push(name || `col${i}`);
    }

    const values: string[][] = [];
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
            break;
          }
          case 6: { // Edge
            const props = mod.ccall("zyx_result_get_props_json", "string", ["number", "number"], [resultPtr, i]);
            row.push(props || "(edge)");
            break;
          }
          case 7: { // List
            row.push("[list]");
            break;
          }
          case 8: { // Map
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
    return { error: null, columns, values, duration: dur };
  }, []);

  const handleRun = useCallback(() => {
    if (status !== "ready") return;
    const mod = moduleRef.current;
    const db = dbRef.current;
    if (!mod || !db) return;

    setError(null);
    setResult(null);
    setDuration(null);

    const res = execQuery(mod, db, query);
    if (res.error) {
      setError(res.error);
    } else {
      setResult({ columns: res.columns, values: res.values });
      setDuration(res.duration);
    }
  }, [status, query, execQuery]);

  const handleKeyDown = useCallback((e: React.KeyboardEvent) => {
    if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
      e.preventDefault();
      handleRun();
    }
  }, [handleRun]);

  return (
    <div className="flex h-full w-full flex-col gap-4 p-4 md:p-8">
      {/* Header */}
      <div className="flex items-center gap-3">
        <div className="flex h-8 w-8 items-center justify-center rounded-lg bg-[rgba(122,144,170,0.15)] border border-[rgba(122,144,170,0.2)]">
          <span className="font-['Space_Mono',monospace] text-[0.65rem] font-bold text-[#94a8be]">
            {">_"}
          </span>
        </div>
        <h2 className="m-0 text-[1.15rem] font-semibold tracking-[0.02em] text-[#e7edf5]">
          {isEn ? "Cypher Playground" : "Cypher 试验场"}
        </h2>
        <div className={`ml-2 flex items-center gap-1.5 rounded-full px-2 py-0.5 text-[0.65rem] font-medium ${
          status === "ready"
            ? "bg-[rgba(74,222,128,0.1)] text-[#4ade80]"
            : status === "loading"
            ? "bg-[rgba(250,204,21,0.1)] text-[#facc15]"
            : "bg-[rgba(248,113,113,0.1)] text-[#f87171]"
        }`}>
          <span className={`inline-block h-1.5 w-1.5 rounded-full ${
            status === "ready" ? "bg-[#4ade80]" : status === "loading" ? "bg-[#facc15] animate-pulse" : "bg-[#f87171]"
          }`} />
          {status === "ready" ? (isEn ? "Ready" : "就绪") : status === "loading" ? (isEn ? "Loading..." : "加载中...") : (isEn ? "Error" : "错误")}
        </div>
      </div>

      {/* Main content area */}
      <div className="flex flex-1 flex-col gap-4 overflow-hidden md:flex-row">
        {/* Left: Editor */}
        <div className="flex flex-1 flex-col gap-3 min-w-0">
          {/* Example query chips */}
          <div className="flex flex-wrap gap-1.5">
            {EXAMPLE_QUERIES.map((eq) => (
              <button
                key={eq.label}
                onClick={() => setQuery(eq.query)}
                className={`rounded-md border px-2.5 py-1 text-[0.7rem] font-medium transition-all duration-200 ${
                  query === eq.query
                    ? "border-[rgba(148,168,190,0.5)] bg-[rgba(122,144,170,0.2)] text-[#e7edf5]"
                    : "border-[rgba(122,144,170,0.15)] bg-[rgba(16,22,30,0.6)] text-[#8a9bb0] hover:border-[rgba(122,144,170,0.3)] hover:text-[#aec0d2]"
                }`}
              >
                {eq.label}
              </button>
            ))}
          </div>

          {/* Editor */}
          <div className="relative flex-1 min-h-[120px]">
            <textarea
              value={query}
              onChange={(e) => setQuery(e.target.value)}
              onKeyDown={handleKeyDown}
              spellCheck={false}
              className="h-full w-full resize-none rounded-lg border border-[rgba(122,144,170,0.15)] bg-[rgba(11,15,20,0.8)] p-4 font-['Space_Mono','Ubuntu_Mono',monospace] text-[0.82rem] leading-relaxed text-[#e7edf5] outline-none transition-colors duration-200 focus:border-[rgba(148,168,190,0.4)] placeholder:text-[#566b82]"
              placeholder={isEn ? "Enter Cypher query..." : "输入 Cypher 查询..."}
            />
            <div className="absolute bottom-3 right-3 flex items-center gap-2">
              <span className="text-[0.6rem] text-[#566b82]">
                {navigator.platform?.includes("Mac") ? "Cmd" : "Ctrl"}+Enter
              </span>
              <button
                onClick={handleRun}
                disabled={status !== "ready"}
                className="rounded-md border border-[rgba(122,144,170,0.5)] bg-[rgba(35,48,63,0.8)] px-4 py-1.5 text-[0.75rem] font-medium text-[#e7edf5] transition-all duration-200 hover:bg-[rgba(122,144,170,0.3)] disabled:opacity-40 disabled:cursor-not-allowed"
              >
                {isEn ? "Run" : "执行"}
              </button>
            </div>
          </div>
        </div>

        {/* Right: Results */}
        <div className="flex flex-1 flex-col gap-2 min-w-0 min-h-[200px] md:min-h-0">
          <div className="flex items-center justify-between">
            <span className="text-[0.7rem] font-medium uppercase tracking-[0.12em] text-[#566b82]">
              {isEn ? "Results" : "结果"}
            </span>
            {duration !== null && (
              <span className="text-[0.65rem] text-[#566b82]">
                {duration.toFixed(2)} ms
              </span>
            )}
          </div>

          <div className="flex-1 overflow-auto rounded-lg border border-[rgba(122,144,170,0.15)] bg-[rgba(11,15,20,0.8)] [scrollbar-width:thin] [scrollbar-color:rgba(122,144,170,0.2)_transparent]">
            {error ? (
              <div className="p-4 text-[0.82rem] text-[#f87171] font-['Space_Mono',monospace]">
                {error}
              </div>
            ) : result ? (
              result.values.length === 0 ? (
                <div className="flex h-full items-center justify-center p-4 text-[0.82rem] text-[#566b82]">
                  {isEn ? "Query executed successfully (no rows returned)" : "查询执行成功（无返回行）"}
                </div>
              ) : (
                <table className="w-full border-collapse text-[0.78rem]">
                  <thead>
                    <tr className="border-b border-[rgba(122,144,170,0.15)]">
                      {result.columns.map((col) => (
                        <th key={col} className="whitespace-nowrap px-4 py-2.5 text-left font-semibold text-[#94a8be]">
                          {col}
                        </th>
                      ))}
                    </tr>
                  </thead>
                  <tbody className="font-['Space_Mono',monospace]">
                    {result.values.map((row, ri) => (
                      <tr key={ri} className="border-b border-[rgba(122,144,170,0.08)] hover:bg-[rgba(122,144,170,0.05)] transition-colors">
                        {row.map((val, ci) => (
                          <td key={ci} className="max-w-[300px] truncate px-4 py-2 text-[#c5d2de]">
                            {val}
                          </td>
                        ))}
                      </tr>
                    ))}
                  </tbody>
                </table>
              )
            ) : (
              <div className="flex h-full items-center justify-center p-4 text-[0.82rem] text-[#566b82]">
                {status === "loading"
                  ? (isEn ? "Loading ZYX engine..." : "正在加载 ZYX 引擎...")
                  : (isEn ? "Run a query to see results" : "执行查询查看结果")}
              </div>
            )}
          </div>
        </div>
      </div>

      {/* Schema hint */}
      <div className="text-[0.65rem] leading-relaxed text-[#566b82]">
        <span className="font-medium text-[#8a9bb0]">Schema:</span>{" "}
        (:Person {"{"} name, age, role {"}"}) -[:KNOWS | :REPORTS_TO | :LIKES]- (:Movie {"{"} title, year, genre {"}"})
      </div>
    </div>
  );
}
