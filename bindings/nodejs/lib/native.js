'use strict';

let addon;
try {
    addon = require('../build/Release/zyxdb.node');
} catch {
    addon = require('../build/Debug/zyxdb.node');
}

module.exports = addon;
