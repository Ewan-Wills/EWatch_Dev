#!/usr/bin/env node
// Called by install.sh. Reads env: SETTINGS (settings.json path), HOOK_SCRIPT,
// STATUSLINE_SCRIPT, ORIGINAL_STATUSLINE_FILE. Writes a timestamped backup next
// to SETTINGS before writing (unless SETTINGS didn't already exist).
'use strict';
const fs = require('fs');
const path = require('path');
const { readJson, mergeHooks, mergeStatusline } = require('./merge.cjs');

const settingsFile = process.env.SETTINGS;
const hookScript = process.env.HOOK_SCRIPT;
const statuslineScript = process.env.STATUSLINE_SCRIPT;
const originalFile = process.env.ORIGINAL_STATUSLINE_FILE;

if (!settingsFile || !hookScript || !statuslineScript || !originalFile) {
  console.error('do-install.cjs: missing required env vars');
  process.exit(1);
}

const existed = fs.existsSync(settingsFile);
const settings = readJson(settingsFile);

if (existed) {
  const stamp = new Date()
    .toISOString()
    .replace(/[-:]/g, '')
    .replace('T', '-')
    .slice(0, 15); // YYYYMMDD-HHMMSS
  // Never overwrite an earlier backup (two runs in the same second would
  // otherwise replace the only copy of the original with the merged file).
  let backup = `${settingsFile}.bak-${stamp}`;
  for (let n = 1; fs.existsSync(backup); n++) backup = `${settingsFile}.bak-${stamp}-${n}`;
  fs.copyFileSync(settingsFile, backup, fs.constants.COPYFILE_EXCL);
  console.error(`backed up ${settingsFile} -> ${backup}`);
}

mergeHooks(settings, hookScript);
mergeStatusline(settings, statuslineScript, originalFile);

fs.mkdirSync(path.dirname(settingsFile), { recursive: true });
fs.writeFileSync(settingsFile, `${JSON.stringify(settings, null, 2)}\n`);
console.error(`wrote ${settingsFile}`);
