// Test the actual static export at the GitHub Pages origin without contacting it.
const assert = require("node:assert/strict");
const { mkdir, stat, writeFile } = require("node:fs/promises");
const { basename, extname, isAbsolute, relative, resolve } = require("node:path");
const { chromium } = require("playwright");

const origin = "https://nexepic.github.io";
const basePath = "/zyx/";
const contentTypes = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".wasm": "application/wasm",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json",
  ".txt": "text/plain; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".ico": "image/x-icon",
  ".woff2": "font/woff2",
};
const [site, legacyFile, evidence] = process.argv.slice(2);

async function locateAsset(root, pathname) {
  const asset = resolve(root, decodeURIComponent(pathname.slice(basePath.length)));
  const rel = relative(root, asset);
  assert(!isAbsolute(rel) && rel !== ".." && !rel.startsWith("../"), `Invalid path: ${pathname}`);
  for (const candidate of [asset, `${asset}.html`, resolve(asset, "index.html")]) {
    if ((await stat(candidate).catch(() => null))?.isFile()) return candidate;
  }
  return null;
}

async function main() {
  assert(site && legacyFile && evidence, "Usage: test-playground.cjs <Pages export> <legacy .zyx file> <evidence dir>");
  const root = resolve(site);
  const currentFile = resolve(root, "data/got.zyx");
  const browser = await chromium.launch();
  try {
    const context = await browser.newContext({ locale: "en-US", serviceWorkers: "block" });
    const requests = new Map();
    const missing = [];
    const pageErrors = [];
    const browserLog = [];
    const page = await context.newPage();
    page.setDefaultTimeout(30_000);
    page.on("pageerror", (error) => pageErrors.push(error.stack ?? String(error)));
    page.on("console", (message) => browserLog.push(`${message.type()}: ${message.text()}`));
    await context.route("**/*", async (route) => {
      const url = new URL(route.request().url());
      if (url.origin !== origin) return route.abort(); // e.g. optional remote fonts
      if (!url.pathname.startsWith(basePath)) {
        missing.push(url.pathname);
        return route.fulfill({ status: 404 });
      }
      const asset = await locateAsset(root, url.pathname);
      if (!asset) {
        missing.push(url.pathname);
        return route.fulfill({ status: 404 });
      }
      const contentType = contentTypes[extname(asset)] ?? "application/octet-stream";
      requests.set(url.pathname, contentType);
      await route.fulfill({ path: asset, contentType });
    });

    const ready = page.getByText("Ready", { exact: true });
    const runQuery = page.getByRole("button", { name: "Run Query", exact: true });
    const editor = page.getByPlaceholder("MATCH (n) RETURN n LIMIT 10...");
    const got = page.getByRole("button", { name: "Game of Thrones", exact: true });
    const imdb = page.getByRole("button", { name: "IMDb Movies", exact: true });
    const fileInput = page.locator('input[type="file"][accept=".zyx"]');
    const queryRows = page.locator("table tbody tr");
    const analytics = page.locator("aside").filter({ has: page.getByText("Analytics", { exact: true }) });

    async function readRows() {
      return queryRows.evaluateAll((trs) => trs.map(
        (tr) => Array.from(tr.cells, (cell) => cell.textContent),
      ));
    }

    async function expectRows(query, columns, rows) {
      await ready.waitFor();
      await editor.fill(query);
      await runQuery.click();
      await page.getByRole("columnheader", { name: columns[0], exact: true }).waitFor();
      assert.deepEqual(await page.getByRole("columnheader").allTextContents(), columns, query);
      assert.deepEqual(await readRows(), rows, query);
    }

    async function expectCounts(nodes, edges) {
      await expectRows("MATCH (n) RETURN count(n) AS nodes", ["nodes"], [[String(nodes)]]);
      await expectRows("MATCH ()-[r]->() RETURN count(r) AS edges", ["edges"], [[String(edges)]]);
    }

    async function expectAlgorithms({ dataset, nodes, components, path }) {
      const algorithms = [
        {
          name: "PageRank", procedure: "pageRank.stream", column: "score",
          params: [["20", "8"], ["0.85", "0.85"]],
        },
        { name: "Connected Components", procedure: "wcc.stream", column: "componentId" },
        {
          name: "Betweenness Centrality", procedure: "betweenness.stream", column: "score",
          params: dataset === "IMDb" ? [["0 (all)", "16"]] : [],
        },
        { name: "Closeness Centrality", procedure: "closeness.stream", column: "score" },
        {
          name: "Shortest Path", procedure: "shortestPath.dijkstra.stream", column: "cost",
          params: [["1", String(path[0])], ["10", String(path[1])]],
        },
      ];
      for (const { name, procedure, column, params = [] } of algorithms) {
        await analytics.getByText(name, { exact: true }).click();
        for (const [placeholder, value] of params) {
          await analytics.getByPlaceholder(placeholder, { exact: true }).fill(value);
        }
        // Run PageRank twice without reloading the database: first creation
        // and replacement of an existing projection must both work.
        for (let iteration = 0; iteration < (name === "PageRank" ? 2 : 1); iteration++) {
          const scenario = `${dataset} — ${name} (run ${iteration + 1})`;
          // Deliberately replace the previous GDS result with a scalar query;
          // stale algorithm output must not satisfy the assertions.
          await expectRows("MATCH (n) RETURN count(n) AS before_algorithm", ["before_algorithm"], [[String(nodes)]]);
          const run = analytics.getByRole("button", { name: `Run ${name}`, exact: true });
          await run.click();
          await run.waitFor(); // Wait for the Running label to return to Run.
          assert.deepEqual(await page.getByRole("alert").allTextContents(), [], scenario);
          assert((await editor.inputValue()).startsWith(`CALL gds.${procedure}(`), scenario);
          if (column !== "componentId") {
            assert(await page.getByRole("button", { name: "Graph", exact: true }).isEnabled(), scenario);
          }
          await page.getByRole("button", { name: "Table", exact: true }).click();
          await page.getByRole("columnheader", { name: column, exact: true }).waitFor();
          assert.deepEqual(await page.getByRole("columnheader").allTextContents(), ["nodeId", column], scenario);
          const rows = (await readRows()).map(([id, value]) => [Number(id), Number(value)]);
          if (column === "cost") {
            // First directed edge of the fixture is a known unit-cost path.
            assert.deepEqual(rows, [[path[0], 0], [path[1], 1]], scenario);
            continue;
          }
          assert.equal(rows.length, nodes, scenario);
          assert(rows.every(([id, value]) => Number.isInteger(id) && id > 0 && Number.isFinite(value) && value >= 0), scenario);
          assert.equal(new Set(rows.map(([id]) => id)).size, nodes, scenario);
          if (column === "componentId") {
            const byNode = new Map(rows);
            const componentIds = new Set(byNode.values());
            assert.equal(componentIds.size, components, scenario);
            for (const id of componentIds) assert.equal(byNode.get(id), id, scenario);
          } else if (name === "PageRank") {
            const sum = rows.reduce((total, [, value]) => total + value, 0);
            assert(Math.abs(sum - 1) < 1e-6, scenario);
          } else {
            assert(rows.some(([, value]) => value > 0), scenario);
          }
        }
      }
    }

    async function expectInvalidPathAndScopeReset() {
      // The valid Shortest Path test above leaves that algorithm selected.
      const start = analytics.getByPlaceholder("1", { exact: true });
      const end = analytics.getByPlaceholder("10", { exact: true });
      const run = analytics.getByRole("button", { name: "Run Shortest Path", exact: true });
      await start.fill("0");
      await run.click();
      await analytics.getByRole("alert").filter({ hasText: /start node ID must be an integer/i }).waitFor();
      await start.fill("1");
      await end.fill("15"); // GoT nodes 1 and 15 exist but have no directed path.
      await run.click();
      await page.getByText("No path found between the specified nodes.", { exact: true }).waitFor();
      assert.deepEqual(await page.getByRole("alert").allTextContents(), [], "No path is not an execution error");

      // Scalar queries do not populate the panel's schema; run a graph query
      // with known labels/types before selecting a non-default scope.
      await editor.fill("MATCH (a:Character)-[r:KILLED]->(b:Character) RETURN a, r, b LIMIT 1");
      await runQuery.click();
      await analytics.getByRole("button", { name: "Character", exact: true }).click();
      await analytics.getByRole("button", { name: "KILLED", exact: true }).click();
      await analytics.getByText("Scope: Character + KILLED", { exact: true }).waitFor();
      await imdb.click();
      await ready.waitFor();
      await analytics.getByText("Scope: Entire graph", { exact: true }).waitFor();
    }

    try {
      await page.goto(`${origin}${basePath}en/playground`, { waitUntil: "domcontentloaded" });
      await expectCounts(428, 562);
      await expectAlgorithms({ dataset: "GoT", nodes: 428, components: 135, path: [16, 10] });
      await expectInvalidPathAndScopeReset();
      await expectCounts(1959, 3092);
      await expectRows(
        "MATCH (d:Person)-[r:DIRECTED]->(m:Movie) RETURN id(r) AS edge ORDER BY edge LIMIT 3",
        ["edge"], [["1"], ["2"], ["3"]],
      );
      await expectAlgorithms({ dataset: "IMDb", nodes: 1959, components: 3, path: [1193, 1560] });
      await got.click();
      await expectCounts(428, 562);

      await fileInput.setInputFiles(currentFile);
      await page.getByRole("button", { name: basename(currentFile), exact: true }).waitFor();
      await expectCounts(428, 562);
      await got.click();
      await expectCounts(428, 562);

      await fileInput.setInputFiles(legacyFile);
      const error = page.getByRole("alert").filter({ hasText: /not a complete native relocatable database/i });
      await error.waitFor();
      assert(await runQuery.isDisabled(), "Rejected file must not offer an active query");
      assert.equal(await got.getAttribute("aria-pressed"), "true");
      await got.click(); // same selected dataset must recover after the error
      await expectCounts(428, 562);
      assert.equal(await error.count(), 0);

      for (const [path, contentType] of [
        ["wasm/zyx.js", contentTypes[".js"]],
        ["wasm/zyx-filesystem.mjs", contentTypes[".mjs"]],
        ["wasm/zyx.wasm", contentTypes[".wasm"]],
        ["data/got.zyx", "application/octet-stream"],
        ["data/imdb.zyx", "application/octet-stream"],
      ]) assert.equal(requests.get(basePath + path), contentType, path);
      assert.deepEqual(missing, [], "Missing same-origin static site assets");
      assert.deepEqual(pageErrors, [], "Uncaught browser errors");
      console.log("Playground verified: both database counts, ordered query, all five algorithms on both datasets, repeated projection, invalid/no-path inputs, scope reset, current-format import, legacy rejection, same-dataset recovery.");
    } catch (error) {
      await mkdir(evidence, { recursive: true });
      await writeFile(resolve(evidence, "browser.json"), JSON.stringify({
        requests: Array.from(requests), missing, pageErrors, browserLog,
      }, null, 2));
      await page.screenshot({ path: resolve(evidence, "playground.png"), timeout: 5_000 }).catch(() => {});
      throw error;
    }
  } finally {
    await browser.close();
  }
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
