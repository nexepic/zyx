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

  // Close to flush data to main file
  mod.ccall("zyx_close", null, ["number"], [db]);

  // If WAL file exists in MEMFS, reopen the database to trigger WAL recovery
  // which merges WAL data into the main file, then close again.
  const walPath = dbPath + "-wal";
  let hasWal = false;
  try {
    const walStat = mod.FS.stat(walPath);
    hasWal = walStat.size > 0;
  } catch {
    // No WAL file
  }

  if (hasWal) {
    console.log(`  WAL detected, reopening to trigger recovery...`);
    const db2 = mod.ccall("zyx_open", "number", ["string"], [dbPath]);
    if (db2) {
      // Begin a read-only transaction to trigger ensureWALAndTransactionManager → recovery
      const txn = mod.ccall("zyx_begin_read_only_transaction", "number", ["number"], [db2]);
      if (txn) {
        mod.ccall("zyx_txn_close", null, ["number"], [txn]);
      }
      mod.ccall("zyx_close", null, ["number"], [db2]);
    }
  }

  // Read the database file from MEMFS
  const data = mod.FS.readFile(dbPath);
  console.log(`  Database size: ${(data.length / 1024).toFixed(1)} KB`);

  // Verify WAL is gone or empty after recovery
  try {
    const walData = mod.FS.readFile(walPath);
    if (walData.length > 32) {
      console.warn(`  Warning: WAL still has ${(walData.length / 1024).toFixed(1)} KB after recovery`);
    }
  } catch {
    // No WAL file — expected
  }

  // Clean up MEMFS
  try {
    mod.FS.unlink(dbPath);
  } catch {}
  try {
    mod.FS.unlink(walPath);
  } catch {}

  return { data };
}

async function main() {
  console.log("=== ZYX Playground Database Builder ===");

  // Dynamically import seed data (TypeScript files)
  // These need to be compiled first or run via bun/tsx
  let GOT_SEED_QUERIES, IMDB_SEED_QUERIES;

  try {
    const gotMod = await import(
      "../docs/apps/docs/home/data/got-seed.ts"
    );
    GOT_SEED_QUERIES = gotMod.GOT_SEED_QUERIES;

    const imdbMod = await import(
      "../docs/apps/docs/home/data/imdb-seed.ts"
    );
    IMDB_SEED_QUERIES = imdbMod.IMDB_SEED_QUERIES;
  } catch (e) {
    console.error(
      "Failed to import seed data. Run with bun or tsx for TypeScript support."
    );
    console.error(e.message);
    process.exit(1);
  }

  console.log(
    `GoT queries: ${GOT_SEED_QUERIES.length}, IMDb queries: ${IMDB_SEED_QUERIES.length}`
  );

  // Load WASM module
  console.log("\nLoading WASM module...");
  const mod = await loadWasmModule();
  console.log("WASM module loaded.");

  // Create output directory
  mkdirSync(OUT_DIR, { recursive: true });

  // Build GoT database
  const got = await buildDatabase(mod, "/got.zyx", GOT_SEED_QUERIES, "Game of Thrones");
  writeFileSync(join(OUT_DIR, "got.zyx"), got.data);

  // Build IMDb database
  const imdb = await buildDatabase(
    mod,
    "/imdb.zyx",
    IMDB_SEED_QUERIES,
    "IMDb Movies"
  );
  writeFileSync(join(OUT_DIR, "imdb.zyx"), imdb.data);

  console.log(`\nDone! Files written to ${OUT_DIR}/`);
  console.log("  got.zyx - Game of Thrones character network");
  console.log("  imdb.zyx - IMDb Movies dataset");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
