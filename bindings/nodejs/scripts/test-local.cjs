'use strict';

const fs = require('node:fs');
const path = require('node:path');

const testArgs = process.argv.slice(2);
const testDir = path.resolve(__dirname, '..', 'test');
const testTargets = testArgs.length > 0
  ? testArgs
  : fs.readdirSync(testDir)
    .filter((file) => file.endsWith('.test.js'))
    .map((file) => path.join(testDir, file));

require('node:child_process').execFileSync(
  process.execPath,
  ['--require', require.resolve('./force-local-native.cjs'), '--test', ...testTargets],
  { stdio: 'inherit' },
);
