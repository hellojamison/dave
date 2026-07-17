#!/usr/bin/env node

const fs = require('node:fs');
const path = require('node:path');
const {
  getVaultRoot,
  buildRoute,
  currentDateParts,
  getConfig,
  dumpFrontmatter,
  splitFrontmatter,
  linkForRel,
  getNoteTitle,
  noteKeyForRel
} = require('./lib/project-notes-graph.cjs');

function printHelp() {
  return `Project notes helper

Usage:
  node scripts/project-notes.cjs route "matisse dark mode buttons" [--json]
  node scripts/project-notes.cjs new --title "Task title" --process theme-qa [--summary "..."] [--type task|evidence] [--runbook "..."]
  node scripts/project-notes.cjs closeout --note "Project Notes/Evidence/2026-07-03 Task title.md" --working "..." --verified "..." --not-verified "..."
`;
}

function parseArgs(argv) {
  const parsed = { _: [] };
  const booleanFlags = new Set(['json', 'help']);
  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    if (!arg.startsWith('--')) {
      parsed._.push(arg);
      continue;
    }
    const equalsIndex = arg.indexOf('=');
    if (equalsIndex !== -1) {
      parsed[arg.slice(2, equalsIndex)] = arg.slice(equalsIndex + 1);
      continue;
    }
    const key = arg.slice(2);
    if (booleanFlags.has(key)) {
      parsed[key] = true;
      continue;
    }
    const next = argv[index + 1];
    if (next && !next.startsWith('--')) {
      parsed[key] = next;
      index += 1;
    } else {
      parsed[key] = true;
    }
  }
  return parsed;
}

function requireArg(args, name) {
  const value = args[name];
  if (typeof value !== 'string' || value.trim() === '') {
    throw new Error(`Missing required --${name}`);
  }
  return value.trim();
}

function sanitizeFileTitle(title) {
  const sanitized = String(title || '')
    .replace(/[\\/:*?"<>|[\]#^\r\n]/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
  return sanitized || 'Untitled Notes Task';
}

function uniqueNotePath(vaultRoot, dateStamp, title) {
  const baseName = `${dateStamp} ${sanitizeFileTitle(title)}`;
  const dirPath = path.join(vaultRoot, 'Evidence');
  for (let counter = 1; counter < 1000; counter += 1) {
    const suffix = counter === 1 ? '' : ` ${counter}`;
    const candidate = path.join(dirPath, `${baseName}${suffix}.md`);
    if (!fs.existsSync(candidate)) {
      return candidate;
    }
  }
  throw new Error(`Could not find an available note path for "${title}"`);
}

function insertIntoSection(body, sectionName, content) {
  if (!content) {
    return body;
  }
  const heading = `## ${sectionName}`;
  const index = body.indexOf(heading);
  if (index === -1) {
    return body;
  }
  const lineEnd = body.indexOf('\n', index);
  if (lineEnd === -1) {
    return `${body}\n\n${content}\n`;
  }
  return `${body.slice(0, lineEnd + 1)}\n${content}\n${body.slice(lineEnd + 1)}`;
}

function templateForType(vaultRoot, type) {
  const fileName = type === 'evidence' ? 'Evidence Template.md' : 'Task Note Template.md';
  const templatePath = path.join(vaultRoot, 'Templates', fileName);
  if (!fs.existsSync(templatePath)) {
    throw new Error(`Missing note template: ${templatePath}`);
  }
  return fs.readFileSync(templatePath, 'utf8');
}

function buildNoteBody(vaultRoot, type, title, summary) {
  const template = templateForType(vaultRoot, type);
  const { body } = splitFrontmatter(template);
  let nextBody = body.trimStart().replace(/^# .+$/m, `# ${title}`);
  nextBody = insertIntoSection(nextBody, type === 'evidence' ? 'Scope' : 'Goal', summary);
  return nextBody.trimEnd();
}

function appendLine(filePath, initialHeading, line) {
  let text = fs.existsSync(filePath) ? fs.readFileSync(filePath, 'utf8') : `${initialHeading}\n\n`;
  if (!text.endsWith('\n')) {
    text += '\n';
  }
  fs.writeFileSync(filePath, `${text}${line}\n`);
}

function dailyInitialText(date, appLink) {
  const frontmatter = {
    title: date,
    schema_version: 1,
    type: 'daily',
    status: 'active',
    date,
    tags: ['notes/daily'],
    related_apps: [appLink]
  };
  return `---\n${dumpFrontmatter(frontmatter)}\n---\n\n# ${date}`;
}

function routeToJson(route) {
  const noteSummary = (rel) => {
    const note = route.graph.noteByRel.get(rel);
    return { rel, title: getNoteTitle(note, rel) };
  };
  return {
    process: noteSummary(route.processRel),
    runbooks: route.runbookRels.map(noteSummary),
    decisions: route.decisionRels.map(noteSummary),
    evidence: route.evidenceRels.map(noteSummary)
  };
}

function formatRoute(route, options = {}) {
  const config = getConfig(options.env || process.env);
  const appName = config.appName || 'My Project';
  const processTitle = getNoteTitle(route.processNote, route.processRel);
  const runbookTitles = route.runbookRels.map((rel) => getNoteTitle(route.graph.noteByRel.get(rel), rel));
  const decisionTitles = route.decisionRels.map((rel) => getNoteTitle(route.graph.noteByRel.get(rel), rel));
  const evidenceTitles = route.evidenceRels.map((rel) => getNoteTitle(route.graph.noteByRel.get(rel), rel));
  const chain = [
    'Start Here',
    appName,
    processTitle,
    ...runbookTitles,
    ...decisionTitles,
    ...evidenceTitles.slice(0, 1)
  ];
  const lines = [
    chain.join(' -> '),
    '',
    `Process: ${linkForRel(route.processRel, processTitle)}`
  ];
  if (route.runbookRels.length > 0) {
    lines.push('Runbooks:');
    for (const rel of route.runbookRels) {
      lines.push(`- ${linkForRel(rel, getNoteTitle(route.graph.noteByRel.get(rel), rel))}`);
    }
  }
  if (route.decisionRels.length > 0) {
    lines.push('Decisions:');
    for (const rel of route.decisionRels) {
      lines.push(`- ${linkForRel(rel, getNoteTitle(route.graph.noteByRel.get(rel), rel))}`);
    }
  }
  if (route.evidenceRels.length > 0) {
    lines.push('Evidence:');
    for (const rel of route.evidenceRels) {
      lines.push(`- ${linkForRel(rel, getNoteTitle(route.graph.noteByRel.get(rel), rel))}`);
    }
  }
  return `${lines.join('\n')}\n`;
}

function printRoute(args, options = {}) {
  const query = args._.join(' ').trim();
  if (!query) {
    throw new Error('Missing route query');
  }
  const route = buildRoute(query, options);
  if (route.error) {
    throw new Error(route.error);
  }
  return args.json ? `${JSON.stringify(routeToJson(route), null, 2)}\n` : formatRoute(route, options);
}

function frontmatterLinksForRels(route, rels) {
  return rels.map((rel) => linkForRel(rel, getNoteTitle(route.graph.noteByRel.get(rel), rel)));
}

function createNewNote(args, options = {}) {
  const env = options.env || process.env;
  const vaultRoot = getVaultRoot({ env, vaultRoot: options.vaultRoot });
  const config = getConfig(env);
  const appName = options.appName || config.appName || 'My Project';
  const appRel = config.appRel || `Apps/${appName}.md`;
  const appLink = linkForRel(appRel, appName);
  const title = requireArg(args, 'title');
  const processInput = requireArg(args, 'process');
  const type = args.type || 'task';
  if (!['task', 'evidence'].includes(type)) {
    throw new Error('--type must be task or evidence');
  }

  const route = buildRoute(processInput, { env, vaultRoot, runbook: args.runbook });
  if (route.error) {
    throw new Error(route.error);
  }
  if (args.runbook && route.runbookRels.length === 0) {
    throw new Error(`No runbook matched "${args.runbook}"`);
  }

  const now = options.now || new Date();
  const { date, time, timeZoneName } = currentDateParts(now);
  const notePath = uniqueNotePath(vaultRoot, date, title);
  const noteRel = path.relative(vaultRoot, notePath).split(path.sep).join('/');
  const processTitle = getNoteTitle(route.processNote, route.processRel);
  const runbookLinks = frontmatterLinksForRels(route, route.runbookRels);
  const decisionLinks = frontmatterLinksForRels(route, route.decisionRels);
  const frontmatter = {
    title,
    schema_version: 1,
    type,
    status: 'active',
    date,
    tags: [`notes/${type}`],
    app: appName,
    source_of_truth: false,
    last_verified: date,
    confidence: 'medium',
    created_by: 'project-notes-cli',
    related_apps: [appLink],
    related_processes: [linkForRel(route.processRel, processTitle)],
    related_runbooks: runbookLinks,
    related_decisions: decisionLinks
  };
  const body = buildNoteBody(vaultRoot, type, title, args.summary || '');
  const graphLinks = [
    `- App: ${appLink}`,
    `- Process: ${linkForRel(route.processRel, processTitle)}`,
    `- Runbook: ${runbookLinks.join(', ') || 'None selected'}`
  ].join('\n');
  const noteText = `---\n${dumpFrontmatter(frontmatter)}\n---\n\n${body.replace(/## Graph Links[\s\S]*$/m, `## Graph Links\n\n${graphLinks}`)}\n`;

  fs.mkdirSync(path.dirname(notePath), { recursive: true });
  fs.writeFileSync(notePath, noteText);

  const dailyPath = path.join(vaultRoot, `${date}.md`);
  const runbookText = runbookLinks.length > 0 ? ` and ${runbookLinks.join(', ')}` : '';
  appendLine(
    dailyPath,
    dailyInitialText(date, appLink),
    `- ${time} ${timeZoneName}: Created notes graph task ${linkForRel(noteRel, title)} linked to ${appLink}, ${linkForRel(route.processRel, processTitle)}${runbookText}. Working: task route prepared. Not verified: implementation outcome is not recorded yet.`
  );

  return {
    notePath,
    noteRel,
    dailyPath,
    route: routeToJson(route)
  };
}

function resolveNotePath(input, vaultRoot) {
  const candidates = [];
  if (path.isAbsolute(input)) {
    candidates.push(input);
  } else {
    candidates.push(path.resolve(process.cwd(), input));
    candidates.push(path.join(vaultRoot, input));
    candidates.push(path.join(path.dirname(vaultRoot), input));
  }
  const found = candidates.find((candidate) => fs.existsSync(candidate));
  return found || candidates[0];
}

function realPath(filePath) {
  return fs.realpathSync.native ? fs.realpathSync.native(filePath) : fs.realpathSync(filePath);
}

function isPathWithin(rootPath, candidatePath) {
  const relative = path.relative(rootPath, candidatePath);
  return relative === ''
    || (!relative.startsWith(`..${path.sep}`) && relative !== '..' && !path.isAbsolute(relative));
}

function closeoutNote(args, options = {}) {
  const env = options.env || process.env;
  const vaultRoot = getVaultRoot({ env, vaultRoot: options.vaultRoot });
  const config = getConfig(env);
  const appName = options.appName || config.appName || 'My Project';
  const appRel = config.appRel || `Apps/${appName}.md`;
  const appLink = linkForRel(appRel, appName);
  const noteInput = requireArg(args, 'note');
  const working = requireArg(args, 'working');
  const verified = requireArg(args, 'verified');
  const notVerified = requireArg(args, 'not-verified');
  const notePath = resolveNotePath(noteInput, vaultRoot);
  if (!fs.existsSync(notePath)) {
    throw new Error(`Missing note: ${noteInput}`);
  }
  const realVaultRoot = realPath(vaultRoot);
  const realNotePath = realPath(notePath);
  if (!isPathWithin(realVaultRoot, realNotePath)) {
    throw new Error(`Note is outside vault: ${noteInput}`);
  }
  const noteRel = path.relative(realVaultRoot, realNotePath).split(path.sep).join('/');
  const original = fs.readFileSync(realNotePath, 'utf8');
  const { frontmatter, body } = splitFrontmatter(original);
  if (!frontmatter) {
    throw new Error(`Note is missing frontmatter: ${noteInput}`);
  }
  const now = options.now || new Date();
  const { date, time, timeZoneName } = currentDateParts(now);
  const nextFrontmatter = {
    ...frontmatter,
    status: 'done',
    last_verified: date
  };
  const title = frontmatter.title || path.basename(realNotePath, '.md');
  const closeout = [
    '',
    '',
    `## Closeout ${date} ${time} ${timeZoneName}`,
    '',
    `- Working: ${working}`,
    `- Verified: ${verified}`,
    `- Not verified: ${notVerified}`
  ].join('\n');
  const nextText = `---\n${dumpFrontmatter(nextFrontmatter)}\n---\n${body.trimEnd()}${closeout}\n`;
  fs.writeFileSync(realNotePath, nextText);

  const dailyPath = path.join(vaultRoot, `${date}.md`);
  appendLine(
    dailyPath,
    dailyInitialText(date, appLink),
    `- ${time} ${timeZoneName}: Closed notes graph task ${linkForRel(noteRel, title)}. Working: ${working} Verified: ${verified} Not verified: ${notVerified}`
  );

  return { notePath: realNotePath, noteRel, dailyPath };
}

function main(argv = process.argv.slice(2), options = {}) {
  const [command, ...rest] = argv;
  const args = parseArgs(rest);
  if (!command || command === 'help' || command === '--help' || command === '-h') {
    return printHelp();
  }
  if (command === 'route') {
    return printRoute(args, options);
  }
  if (command === 'new') {
    const result = createNewNote(args, options);
    return `Created ${result.noteRel}\nUpdated ${noteKeyForRel(path.basename(result.dailyPath))}.md\n`;
  }
  if (command === 'closeout') {
    const result = closeoutNote(args, options);
    return `Closed ${result.noteRel}\nUpdated ${noteKeyForRel(path.basename(result.dailyPath))}.md\n`;
  }
  throw new Error(`Unknown command: ${command}`);
}

if (require.main === module) {
  try {
    process.stdout.write(main());
  } catch (error) {
    process.stderr.write(`${error.message}\n`);
    process.exit(1);
  }
}

module.exports = {
  parseArgs,
  printRoute,
  createNewNote,
  closeoutNote,
  main,
  sanitizeFileTitle
};
