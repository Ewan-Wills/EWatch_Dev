import * as path from 'path';

/** Strip anything outside printable ASCII (0x20-0x7e). Matches protocol.gen.ts writeStr behaviour. */
export function stripNonAscii(s: string): string {
  return (s ?? '').replace(/[^\x20-\x7e]/g, '?');
}

const FILE_TOOLS = new Set(['Edit', 'Write', 'Read', 'NotebookEdit']);

/**
 * Formats a tool call into the protocol's "Tool|target" short form.
 * - Bash: first 24 chars of the command's first line.
 * - Edit/Write/Read/NotebookEdit: basename only, never an absolute or relative path.
 * - Everything else: tool name only, no target (so no leakage of arbitrary MCP tool args).
 * Never includes prompt text, full commands, or absolute paths.
 */
export function formatCurrentTool(toolName: string, toolInput: unknown): string {
  const name = stripNonAscii(toolName ?? '').slice(0, 24);
  if (!name) return '';

  if (name === 'Bash' || name === 'PowerShell') {
    const cmd = extractString(toolInput, 'command');
    if (cmd) {
      const firstLine = cmd.split('\n')[0] ?? '';
      const target = stripNonAscii(firstLine).slice(0, 24);
      return joinTarget(name, target);
    }
    return name;
  }

  if (FILE_TOOLS.has(name)) {
    const filePath = extractString(toolInput, 'file_path') ?? extractString(toolInput, 'path');
    if (filePath) {
      const base = stripNonAscii(path.basename(filePath)).slice(0, 24);
      return joinTarget(name, base);
    }
    return name;
  }

  // Others: tool name only (covers arbitrary MCP tools like mcp__server__tool).
  return name;
}

function joinTarget(name: string, target: string): string {
  if (!target) return name;
  // Field is char[24]; leave room for the '|' separator, favour keeping the tool name intact.
  const budget = 24 - name.length - 1;
  if (budget <= 0) return name.slice(0, 24);
  return `${name}|${target.slice(0, budget)}`;
}

function extractString(obj: unknown, key: string): string | null {
  if (obj && typeof obj === 'object' && key in (obj as Record<string, unknown>)) {
    const v = (obj as Record<string, unknown>)[key];
    return typeof v === 'string' ? v : null;
  }
  return null;
}

/** basename-only, ASCII-safe project label (never an absolute path). */
export function projectLabel(repoName: string | null | undefined, projectDir: string | null | undefined): string {
  const name = repoName || (projectDir ? path.basename(projectDir) : '');
  return stripNonAscii(name).slice(0, 16);
}
