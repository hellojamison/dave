// SPDX-License-Identifier: GPL-3.0-or-later
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const notes = require('../scripts/project-notes.cjs');

test('new notes and closeouts leave the shared daily note untouched', (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'dave-notes-test-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const vaultRoot = path.join(root, 'vault');
  fs.mkdirSync(path.join(vaultRoot, 'Templates'), { recursive: true });
  fs.writeFileSync(path.join(vaultRoot, 'Templates', 'App Template.md'), `---
title: App Template
type: template
status: active
---
<!-- notes-graph-kit:scaffold:start -->
\`\`\`yaml
title: App
schema_version: 1
type: app
status: current
date: 2026-01-01
tags: [notes/app]
\`\`\`
<!-- notes-graph-kit:scaffold:end -->
# App
## Scope
`);
  const config = path.join(root, 'config.json');
  fs.writeFileSync(config, JSON.stringify({ appName: 'Dave', appRel: 'Apps/Dave.md', routes: [] }));
  const options = { vaultRoot, env: { ...process.env, PROJECT_NOTES_CONFIG: config },
                    now: new Date('2026-10-31T19:00:00Z') };
  const shared = path.join(vaultRoot, '2026-10-31.md');
  fs.writeFileSync(shared, 'Existing daily note\n');
  const created = notes.createNewNote({ title: 'Example', type: 'app' }, options);
  assert.equal(fs.readFileSync(shared, 'utf8'), 'Existing daily note\n');
  const closed = notes.closeoutNote({ note: created.noteRel, working: 'Ready',
                                     verified: 'Checked', 'not-verified': 'Nothing else' }, options);
  assert.equal(fs.readFileSync(shared, 'utf8'), 'Existing daily note\n');
  assert.match(closed.dailyPath, /Daily\/2026-10-31\//);
  assert.match(fs.readFileSync(closed.dailyPath, 'utf8'), /Closed notes graph task/);
  const second = notes.createNewNote({ title: 'Another', type: 'app' }, options);
  const secondCloseout = notes.closeoutNote({ note: second.noteRel, working: 'Ready',
    verified: 'Checked', 'not-verified': 'Nothing else' }, options);
  assert.notEqual(secondCloseout.dailyPath, closed.dailyPath);
  assert.ok(fs.existsSync(closed.dailyPath));
  assert.equal(fs.readFileSync(shared, 'utf8'), 'Existing daily note\n');
});
