#!/usr/bin/env node
// Called by uninstall.sh. Removes only our hook entries and restores the
// original statusLine command (or removes the key if there wasn't one).
'use strict';
const fs = require('fs');
const { readJson, unmergeHooks, unmergeStatusline } = require('./merge.cjs');

const settingsFile = process.env.SETTINGS;
const originalFile = process.env.ORIGINAL_STATUSLINE_FILE;

if (!settingsFile || !originalFile) {
  console.error('do-uninstall.cjs: missing required env vars');
  process.exit(1);
}

if (!fs.existsSync(settingsFile)) {
  console.error(`${settingsFile} does not exist, nothing to uninstall`);
  process.exit(0);
}

const settings = readJson(settingsFile);
unmergeHooks(settings);
unmergeStatusline(settings, originalFile);

fs.writeFileSync(settingsFile, `${JSON.stringify(settings, null, 2)}\n`);
console.error(`wrote ${settingsFile}`);
