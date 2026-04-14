#!/usr/bin/env node
/**
 * Build pre-built ZYX database files for the Cypher Playground.
 *
 * Uses the WASM module to create databases, seed them with data,
 * then extracts the MEMFS files to disk.
 *
 * Usage:
 *   cd docs/apps/docs
 *   node --experimental-vm-modules ../../scripts/build_playground_db.mjs
 *   # or: bun ../../scripts/build_playground_db.mjs
 */

import { readFileSync, writeFileSync, mkdirSync } from "fs";
import { join, dirname } from "path";
import { fileURLToPath } from "url";
import { createRequire } from "module";

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);
const require = createRequire(import.meta.url);

// Paths
const WASM_DIR = join(__dirname, "../docs/apps/docs/public/wasm");
const OUT_DIR = join(__dirname, "../docs/apps/docs/public/data");

async function loadWasmModule() {
  // The Emscripten module is a CommonJS module
  const createZyxModule = require(join(WASM_DIR, "zyx.js"));
  const mod = await createZyxModule({
    locateFile: (path) => join(WASM_DIR, path),
    print: () => {},
    printErr: (msg) => {
      if (!msg.includes("warning")) console.error("  [wasm]", msg);
    },
  });
  return mod;
}

function execCypher(mod, db, query) {
  const resultPtr = mod.ccall(
    "zyx_execute",
    "number",
    ["number", "string"],
    [db, query]
  );
  if (resultPtr) {
    mod.ccall("zyx_result_close", null, ["number"], [resultPtr]);
  }
}

async function buildDatabase(mod, dbPath, seedQueries, label) {
  console.log(`\nBuilding ${label} database...`);
  console.log(`  Seed queries: ${seedQueries.length}`);

  const db = mod.ccall("zyx_open", "number", ["string"], [dbPath]);
  if (!db) {
    console.error(`  Failed to open database at ${dbPath}`);
    process.exit(1);
  }

  let count = 0;
  for (const q of seedQueries) {
    execCypher(mod, db, q);
    count++;
    if (count % 100 === 0) {
      process.stdout.write(`  Seeded ${count}/${seedQueries.length}\r`);
    }
  }
  console.log(`  Seeded ${count}/${seedQueries.length} queries`);

  // Close to flush WAL
  mod.ccall("zyx_close", null, ["number"], [db]);

  // Read the database file from MEMFS
  const data = mod.FS.readFile(dbPath);
  console.log(`  Database size: ${(data.length / 1024).toFixed(1)} KB`);

  // Check for WAL file
  const walPath = dbPath + "-wal";
  let walData = null;
  try {
    walData = mod.FS.readFile(walPath);
    console.log(`  WAL size: ${(walData.length / 1024).toFixed(1)} KB`);
  } catch {
    // No WAL file (expected after clean close)
  }

  // Clean up MEMFS
  try {
    mod.FS.unlink(dbPath);
  } catch {}
  try {
    mod.FS.unlink(walPath);
  } catch {}

  return { data, walData };
}

async function main() {
  console.log("=== ZYX Playground Database Builder ===");

  // Dynamically import seed data (TypeScript files)
  // These need to be compiled first or run via bun/tsx
  let GOT_SEED_QUERIES, MARVEL_SEED_QUERIES;

  try {
    const gotMod = await import(
      "../docs/apps/docs/home/data/got-seed.ts"
    );
    GOT_SEED_QUERIES = gotMod.GOT_SEED_QUERIES;

    const marvelMod = await import(
      "../docs/apps/docs/home/data/marvel-seed.ts"
    );
    MARVEL_SEED_QUERIES = marvelMod.MARVEL_SEED_QUERIES;
  } catch (e) {
    console.error(
      "Failed to import seed data. Run with bun or tsx for TypeScript support."
    );
    console.error(e.message);
    process.exit(1);
  }

  console.log(
    `GoT queries: ${GOT_SEED_QUERIES.length}, Marvel queries: ${MARVEL_SEED_QUERIES.length}`
  );

  // Load WASM module
  console.log("\nLoading WASM module...");
  const mod = await loadWasmModule();
  console.log("WASM module loaded.");

  // Create output directory
  mkdirSync(OUT_DIR, { recursive: true });

  // Build GoT database
  const got = await buildDatabase(mod, "/got.db", GOT_SEED_QUERIES, "Game of Thrones");
  writeFileSync(join(OUT_DIR, "got.db"), got.data);
  if (got.walData) writeFileSync(join(OUT_DIR, "got.db-wal"), got.walData);

  // Build Marvel database
  const marvel = await buildDatabase(
    mod,
    "/marvel.db",
    MARVEL_SEED_QUERIES,
    "Marvel Universe"
  );
  writeFileSync(join(OUT_DIR, "marvel.db"), marvel.data);
  if (marvel.walData) writeFileSync(join(OUT_DIR, "marvel.db-wal"), marvel.walData);

  console.log(`\nDone! Files written to ${OUT_DIR}/`);
  console.log("  got.db    - Game of Thrones character network");
  console.log("  marvel.db - Marvel Universe hero network");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
