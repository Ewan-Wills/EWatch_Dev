// Frames from the REAL bridge encoder (ClaudeFace/bridge/src/protocol.gen.ts,
// compiled by run.sh into a temp dir) for xcheck.cpp, plus the summary lines
// xcheck must print for them.
//   node gen_frames.js <protocol.gen.js> <frames_out> <expected_out>
'use strict';
const fs = require('fs');
const P = require(process.argv[2]);
const frames = [], expect = [];

function state(over) {
  const b = P.encodeState(Object.assign(P.zeroState(), { version: 1 }, over));
  frames.push(P.frame('S', b));
  const s = P.decodeState(b);   // the encoder's own truncation / clamping
  expect.push(`S seq=${s.seq} g=${s.globalState} p=${s.pState} tool=${s.pCurrentTool} ` +
    `proj=${s.pProject} 5h=${s.fiveUsed} 7d=${s.sevenUsed} ctx=${s.pCtx} ` +
    `lines=${s.pLinesAdded}/${s.pLinesRemoved} last=${s.pLastTurnS} flags=${s.flags}`);
}
state({ seq: 1, globalState: 1, pState: 1, pCurrentTool: '', pProject: 'EWatch', fiveUsed: 12, sevenUsed: 3, pCtx: 41 });
state({ seq: 513, globalState: 3, pState: 3, pCurrentTool: 'Bash|npm test', pProject: 'EWatch_Firmware',
        fiveUsed: 46, sevenUsed: 31, pCtx: 92, pLinesAdded: 214, pLinesRemoved: 37, pLastTurnS: 95, flags: 9 });
state({ seq: 65535, globalState: 2, pState: 5, pCurrentTool: 'Edit|claude_face_draw.cpp', pProject: 'website',
        fiveUsed: 255, sevenUsed: 255, pCtx: 255, pLinesAdded: 65535, pLinesRemoved: 0, pLastTurnS: 3600, flags: 16 });

const row = (project, state, ctx) => ({ state, ctx, project, turnElapsedS: 0 });
const rows = [row('claude-code', 2, 40), row('website', 1, 12), row('scrubmarine', 4, 255), row('x', 0, 0)];
for (const n of [0, 1, 4]) {
  const m = { version: 1, seq: 7 + n, count: n, rows: rows.map((r, i) => (i < n ? r : row('', 0, 0))) };
  frames.push(P.frame('L', P.encodeSessions(m)));
  expect.push(`L count=${n} row0=${n ? rows[0].project : ''}`);
}

for (const [epoch, off] of [[1790164800, 60], [1790121600, -300], [4102444799, 840]]) {
  frames.push(P.frame('T', P.encodeTime({ version: 1, epoch, utcOffsetMin: off })));
  expect.push(`T epoch=${epoch} off=${off}`);
}

// A CRC-valid frame from a future protocol version: parsed as VERSION.
const future = P.encodeTime({ version: 2, epoch: 1790164800, utcOffsetMin: 0 });
frames.push(P.frame('T', future));
expect.push('VERSION v=2');

fs.writeFileSync(process.argv[3], frames.join(''));
fs.writeFileSync(process.argv[4], expect.join('\n') + '\n');
console.log(`gen_frames: ${frames.length} frames`);
