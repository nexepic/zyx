'use strict';

const Module = require('node:module');

const platformPackages = new Set([
  '@nexepic/zyxdb-darwin-arm64',
  '@nexepic/zyxdb-linux-x64-gnu',
  '@nexepic/zyxdb-win32-x64-msvc',
]);

const originalLoad = Module._load;
Module._load = function forceLocalNativeAddon(request, parent, isMain) {
  if (platformPackages.has(request)) {
    throw new Error('Use the local development addon build for tests');
  }
  return originalLoad.apply(this, arguments);
};
