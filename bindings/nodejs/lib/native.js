'use strict';

const { platform, arch } = process;

const PLATFORMS = {
  'linux-x64': '@nexepic/zyxdb-linux-x64-gnu',
  'darwin-arm64': '@nexepic/zyxdb-darwin-arm64',
  'win32-x64': '@nexepic/zyxdb-win32-x64-msvc',
};

const key = `${platform}-${arch}`;
const pkg = PLATFORMS[key];
if (!pkg) {
  throw new Error(`Unsupported platform: ${key}. Supported: ${Object.keys(PLATFORMS).join(', ')}`);
}

let addon;
try {
  addon = require(pkg);
} catch (e) {
  // Fallback to local build (for development)
  try {
    addon = require('../build/Release/zyxdb.node');
  } catch {
    addon = require('../build/Debug/zyxdb.node');
  }
}

module.exports = addon;
