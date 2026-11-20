// Test the actual static export at the GitHub Pages origin without contacting it.
const assert = require("node:assert/strict");
const { mkdir, readFile, stat, writeFile } = require("node:fs/promises");
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

    async function expectRows(query, columns, rows) {
      await ready.waitFor();
      await editor.fill(query);
      await runQuery.click();
      await page.getByRole("columnheader", { name: columns[0], exact: true }).waitFor();
      assert.deepEqual(await page.getByRole("columnheader").allTextContents(), columns, query);
      assert.deepEqual(await queryRows.evaluateAll((trs) => trs.map(
        (tr) => Array.from(tr.cells, (cell) => cell.textContent),
      )), rows, query);
    }

    async function expectCounts(nodes, edges) {
      await expectRows("MATCH (n) RETURN count(n) AS nodes", ["nodes"], [[String(nodes)]]);
      await expectRows("MATCH ()-[r]->() RETURN count(r) AS edges", ["edges"], [[String(edges)]]);
    }

    try {
      await page.goto(`${origin}${basePath}en/playground`, { waitUntil: "domcontentloaded" });
      await expectCounts(428, 562);
      await imdb.click();
      await expectCounts(1959, 3092);
      await expectRows(
        "MATCH (d:Person)-[r:DIRECTED]->(m:Movie) RETURN id(r) AS edge ORDER BY edge LIMIT 3",
        ["edge"], [["1"], ["2"], ["3"]],
      );
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
      console.log("Playground verified: both database counts, ordered query, current-format import, legacy rejection, same-dataset recovery.");
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
