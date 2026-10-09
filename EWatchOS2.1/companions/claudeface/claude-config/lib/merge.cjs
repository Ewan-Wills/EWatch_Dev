#!/usr/bin/env node
// Pure logic for merging/unmerging claude-bridge's hooks + statusLine into a
// Claude Code settings.json. No side effects beyond the file paths passed in —
// this is what install.sh/uninstall.sh call, and what the test harness calls
// directly against temp copies.
'use strict';
const fs = require('fs');
const path = require('path');

const HOOK_MARKER = 'claude-bridge-hook';
const STATUSLINE_MARKER = 'claude-bridge-statusline';

// Every hook event we wire up (SPEC 1B + docs-verification.md corrections).
const HOOK_EVENTS = [
  'SessionStart',
  'UserPromptSubmit',
  'PreToolUse',
  'PostToolUse',
  'PostToolUseFailure',
  'PermissionRequest',
  'PermissionDenied',
  'Notification',
  'Stop',
  'StopFailure',
  'SubagentStart',
  'SubagentStop',
  'PreCompact',
  'PostCompact',
  'SessionEnd',
];

function readJson(file) {
  try {
    const raw = fs.readFileSync(file, 'utf8');
    if (!raw.trim()) return {};
    return JSON.parse(raw);
  } catch (err) {
    if (err.code === 'ENOENT') return {};
    throw new Error(`could not parse ${file} as JSON: ${err.message}`);
  }
}

function isOurHook(entry) {
  return typeof entry?.command === 'string' && entry.command.includes(HOOK_MARKER);
}

function isOurStatusline(command) {
  return typeof command === 'string' && command.includes(STATUSLINE_MARKER);
}

/**
 * Merge our hook entries into settings.hooks, one entry per event, tagged by
 * command path containing HOOK_MARKER so uninstall can find them again.
 * Idempotent: running twice does not duplicate entries.
 */
function mergeHooks(settings, hookScriptPath) {
  settings.hooks = settings.hooks || {};
  for (const event of HOOK_EVENTS) {
    const bucket = settings.hooks[event] || [];
    // A "matcher group" for our hook: { hooks: [{ type: command, command, async: true }] }
    // Find an existing matcher group that already contains our hook (any matcher shape).
    let alreadyPresent = false;
    for (const group of bucket) {
      if (Array.isArray(group.hooks) && group.hooks.some(isOurHook)) {
        alreadyPresent = true;
        // Keep the command path current (e.g. after a re-install to a new location).
        group.hooks = group.hooks.map((h) => (isOurHook(h) ? { ...h, command: hookScriptPath, async: true } : h));
      }
    }
    if (!alreadyPresent) {
      bucket.push({
        hooks: [{ type: 'command', command: hookScriptPath, async: true, timeout: 5 }],
      });
    }
    settings.hooks[event] = bucket;
  }
  return settings;
}

/** Removes only OUR hook entries; leaves everything else (including empty
 *  matcher groups the user had) untouched. */
function unmergeHooks(settings) {
  if (!settings.hooks) return settings;
  for (const event of Object.keys(settings.hooks)) {
    const bucket = settings.hooks[event];
    if (!Array.isArray(bucket)) continue;
    const filtered = bucket
      .map((group) => {
        if (!Array.isArray(group.hooks)) return group;
        const hooks = group.hooks.filter((h) => !isOurHook(h));
        return { ...group, hooks };
      })
      .filter((group) => !Array.isArray(group.hooks) || group.hooks.length > 0);
    if (filtered.length > 0) {
      settings.hooks[event] = filtered;
    } else {
      delete settings.hooks[event];
    }
  }
  if (Object.keys(settings.hooks).length === 0) delete settings.hooks;
  return settings;
}

/**
 * Sets statusLine to our wrapper script, saving any pre-existing command to
 * originalFile first (only if we haven't already saved one, and only if the
 * existing command isn't already ours — keeps re-installs idempotent).
 */
function mergeStatusline(settings, statuslineScriptPath, originalFile) {
  const existing = settings.statusLine;
  if (existing && existing.command && !isOurStatusline(existing.command)) {
    fs.mkdirSync(path.dirname(originalFile), { recursive: true });
    if (!fs.existsSync(originalFile)) {
      fs.writeFileSync(originalFile, existing.command);
    }
  }
  settings.statusLine = {
    type: 'command',
    command: statuslineScriptPath,
    refreshInterval: 30,
  };
  return settings;
}

/** Restores the original statusLine command if we saved one; otherwise removes
 *  the key entirely (there was none before we installed). */
function unmergeStatusline(settings, originalFile) {
  if (!settings.statusLine || !isOurStatusline(settings.statusLine.command || '')) {
    return settings; // not ours (or absent) — leave as-is
  }
  if (fs.existsSync(originalFile)) {
    const original = fs.readFileSync(originalFile, 'utf8');
    settings.statusLine = { type: 'command', command: original };
  } else {
    delete settings.statusLine;
  }
  return settings;
}

module.exports = {
  HOOK_EVENTS,
  HOOK_MARKER,
  STATUSLINE_MARKER,
  readJson,
  isOurHook,
  isOurStatusline,
  mergeHooks,
  unmergeHooks,
  mergeStatusline,
  unmergeStatusline,
};
