const fs = require('node:fs');
const path = require('node:path');
const yaml = require('js-yaml');

const defaultRepoRoot = path.resolve(__dirname, '..', '..');

const allowedTypes = new Set([
  'index',
  'app',
  'task',
  'process',
  'runbook',
  'decision',
  'incident',
  'evidence',
  'daily',
  'release',
  'audit',
  'known-good',
  'template'
]);

const allowedStatuses = new Set([
  'draft',
  'active',
  'in-progress',
  'blocked',
  'verified',
  'stale',
  'superseded',
  'partial',
  'current',
  'done',
  'complete',
  'implemented',
  'investigating',
  'investigated',
  'fixed-uncommitted',
  'packaged',
  'archived'
]);
const allowedConfidence = new Set(['high', 'medium', 'low']);

const relationshipTypeExpectations = {
  related_apps: new Set(['app']),
  related_processes: new Set(['process']),
  related_runbooks: new Set(['runbook']),
  related_decisions: new Set(['decision']),
  related_incidents: new Set(['incident']),
  related_evidence: new Set(['evidence', 'audit', 'incident', 'release'])
};

const structuredFolders = new Set([
  '_Codex',
  'Apps',
  'Processes',
  'Runbooks',
  'Decisions',
  'Incidents',
  'Evidence',
  'Releases'
]);

const defaultRouteDefinitions = [
  {
    id: 'branch-sync',
    processRel: 'Processes/Branch Sync.md',
    aliases: [
      'branch',
      'sync',
      'merge',
      'main',
      'worktree',
      'branch sync'
    ]
  },
  {
    id: 'release-packaging',
    processRel: 'Processes/Release Packaging.md',
    aliases: [
      'release',
      'package',
      'packaging',
      'notarize',
      'notarization',
      'updater',
      'deploy'
    ]
  },
  {
    id: 'quality-qa',
    processRel: 'Processes/Quality QA.md',
    aliases: [
      'qa',
      'quality',
      'test',
      'tests',
      'bug',
      'regression',
      'review',
      'app',
      'product'
    ]
  },
  {
    id: 'notes-graph-maintenance',
    processRel: 'Processes/Notes Graph Maintenance.md',
    aliases: [
      'notes',
      'obsidian',
      'graph',
      'vault',
      'notes graph'
    ]
  }
];

function getConfig(env = process.env) {
  const repoRoot = path.resolve(env.PROJECT_NOTES_NOTES_REPO_ROOT || defaultRepoRoot);
  const configPath = path.resolve(env.PROJECT_NOTES_CONFIG || path.join(repoRoot, 'notes-graph.config.json'));
  if (!fs.existsSync(configPath)) {
    return {};
  }
  try {
    return JSON.parse(fs.readFileSync(configPath, 'utf8')) || {};
  } catch (error) {
    throw new Error(`Invalid notes graph config at ${configPath}: ${error.message}`);
  }
}

function getRouteDefinitions(env = process.env) {
  const config = getConfig(env);
  return Array.isArray(config.routes) && config.routes.length > 0
    ? config.routes
    : defaultRouteDefinitions;
}

function getRepoRoot(env = process.env) {
  return path.resolve(env.PROJECT_NOTES_NOTES_REPO_ROOT || defaultRepoRoot);
}

function getVaultRoot(options = {}) {
  const env = options.env || process.env;
  return path.resolve(
    options.vaultRoot
      || env.PROJECT_NOTES_NOTES_VAULT_ROOT
      || path.join(getRepoRoot(env), getConfig(env).vaultDir || "Project Notes")
  );
}

function toPosix(value) {
  return value.split(path.sep).join('/');
}

function relativePath(filePath, vaultRoot) {
  return toPosix(path.relative(vaultRoot, filePath));
}

function walk(dirPath, predicate = () => true) {
  const entries = [];
  if (!fs.existsSync(dirPath)) {
    return entries;
  }
  for (const entry of fs.readdirSync(dirPath, { withFileTypes: true })) {
    if (entry.name.startsWith('.')) {
      continue;
    }
    const entryPath = path.join(dirPath, entry.name);
    if (entry.isDirectory()) {
      entries.push(...walk(entryPath, predicate));
    } else if (predicate(entryPath)) {
      entries.push(entryPath);
    }
  }
  return entries;
}

function parseMarkdown(filePath, vaultRoot) {
  const text = fs.readFileSync(filePath, 'utf8');
  const rel = relativePath(filePath, vaultRoot);
  if (!text.startsWith('---\n')) {
    return { rel, filePath, text, frontmatter: null, body: text, frontmatterError: null };
  }
  const endIndex = text.indexOf('\n---\n', 4);
  if (endIndex === -1) {
    return {
      rel,
      filePath,
      text,
      frontmatter: null,
      body: text,
      frontmatterError: 'frontmatter block is not closed'
    };
  }
  const rawFrontmatter = text.slice(4, endIndex);
  try {
    const frontmatter = yaml.load(rawFrontmatter) || {};
    return {
      rel,
      filePath,
      text,
      frontmatter,
      body: text.slice(endIndex + 5),
      frontmatterError: null
    };
  } catch (error) {
    return {
      rel,
      filePath,
      text,
      frontmatter: null,
      body: text.slice(endIndex + 5),
      frontmatterError: `invalid YAML frontmatter: ${error.message}`
    };
  }
}

function noteKeyForRel(rel) {
  return rel.replace(/\.(md|base)$/i, '');
}

function buildNoteIndex(filePaths, vaultRoot) {
  const byPath = new Map();
  const byBasename = new Map();
  for (const filePath of filePaths) {
    const rel = relativePath(filePath, vaultRoot);
    const key = noteKeyForRel(rel);
    const basename = path.basename(key);
    byPath.set(key.toLowerCase(), rel);
    byBasename.set(basename.toLowerCase(), rel);
  }
  return { byPath, byBasename };
}

function buildFrontmatterByRel(notes) {
  const byRel = new Map();
  for (const note of notes) {
    if (note.frontmatter) {
      byRel.set(note.rel, note.frontmatter);
    }
  }
  return byRel;
}

function extractWikilinkTargets(text) {
  const targets = [];
  const pattern = /!?\[\[([^\]\n]+)\]\]/g;
  let match;
  while ((match = pattern.exec(text))) {
    const withoutAlias = match[1].split('|')[0].trim();
    const withoutHeading = withoutAlias.split('#')[0].trim();
    if (withoutHeading) {
      targets.push(withoutHeading);
    }
  }
  return targets;
}

function routeDefinitionLabel(definition, index) {
  return typeof definition?.id === 'string' && definition.id.trim()
    ? `route "${definition.id.trim()}"`
    : `route #${index + 1}`;
}

function routeAliases(definition) {
  return Array.isArray(definition?.aliases)
    ? definition.aliases.filter((alias) => typeof alias === 'string' && alias.trim())
    : [];
}

function isUsableRouteDefinition(definition) {
  return Boolean(
    definition
    && typeof definition === 'object'
    && !Array.isArray(definition)
    && typeof definition.id === 'string'
    && definition.id.trim()
    && typeof definition.processRel === 'string'
    && definition.processRel.trim()
  );
}

function validateRouteDefinitions(definitions, graph = null, options = {}) {
  const errors = [];
  if (!Array.isArray(definitions)) {
    return ['notes-graph.config.json: routes must be an array'];
  }
  const requireExistingProcessTargets = Boolean(options.requireExistingProcessTargets);

  definitions.forEach((definition, index) => {
    const label = routeDefinitionLabel(definition, index);
    if (!definition || typeof definition !== 'object' || Array.isArray(definition)) {
      errors.push(`${label}: must be an object`);
      return;
    }
    if (typeof definition.id !== 'string' || !definition.id.trim()) {
      errors.push(`${label}: id must be a non-empty string`);
    }
    if (typeof definition.processRel !== 'string' || !definition.processRel.trim()) {
      errors.push(`${label}: processRel must be a non-empty string`);
    }
    if (definition.aliases != null && !Array.isArray(definition.aliases)) {
      errors.push(`${label}: aliases must be an array of strings`);
    } else if (Array.isArray(definition.aliases)) {
      definition.aliases.forEach((alias, aliasIndex) => {
        if (typeof alias !== 'string' || !alias.trim()) {
          errors.push(`${label}: aliases[${aliasIndex}] must be a non-empty string`);
        }
      });
    }

    if (!graph || typeof definition.processRel !== 'string' || !definition.processRel.trim()) {
      return;
    }
    const resolved = resolveTarget(definition.processRel, graph.index);
    if (!resolved) {
      if (requireExistingProcessTargets) {
        errors.push(`${label}: processRel ${definition.processRel} must target an existing process note`);
      }
      return;
    }
    const targetFrontmatter = graph.frontmatterByRel?.get(resolved)
      || graph.noteByRel?.get(resolved)?.frontmatter;
    if (targetFrontmatter?.type !== 'process') {
      errors.push(
        `${label}: processRel ${definition.processRel} must target type process; found ${targetFrontmatter?.type || 'missing type'}`
      );
    }
  });

  return errors;
}

function validateRouteConfig(config = {}, graph = null) {
  if (config.routes != null && !Array.isArray(config.routes)) {
    return ['notes-graph.config.json: routes must be an array'];
  }
  const hasConfiguredRoutes = Array.isArray(config.routes) && config.routes.length > 0;
  const definitions = hasConfiguredRoutes ? config.routes : defaultRouteDefinitions;
  return validateRouteDefinitions(definitions, graph, {
    requireExistingProcessTargets: hasConfiguredRoutes
  });
}

function findMalformedWikilinks(text) {
  const source = String(text || '');
  const malformed = [];
  let index = 0;

  while ((index = source.indexOf('[[', index)) !== -1) {
    const lineEnd = source.indexOf('\n', index);
    const scanEnd = lineEnd === -1 ? source.length : lineEnd;
    const closeIndex = source.indexOf(']]', index + 2);
    if (closeIndex === -1 || closeIndex > scanEnd) {
      malformed.push(source.slice(index, scanEnd).trim());
      index += 2;
      continue;
    }

    const inner = source.slice(index + 2, closeIndex);
    if (!inner.trim() || inner.includes('[') || inner.includes(']')) {
      malformed.push(source.slice(index, closeIndex + 2));
    }
    index = closeIndex + 2;
  }

  return malformed;
}

function resolveTarget(target, index) {
  if (typeof target !== 'string' || !target.trim()) {
    return null;
  }
  const normalized = target.replace(/\.(md|base)$/i, '').replace(/\\/g, '/').toLowerCase();
  if (index.byPath.has(normalized)) {
    return index.byPath.get(normalized);
  }
  const basename = path.basename(normalized);
  if (index.byBasename.has(basename)) {
    return index.byBasename.get(basename);
  }
  return null;
}

function firstFolder(rel) {
  return rel.split('/')[0];
}

function isTemplate(rel) {
  return rel.startsWith('Templates/');
}

function isDaily(rel) {
  return /^\d{4}-\d{2}-\d{2}\.md$/.test(rel);
}

function isStructured(rel) {
  return structuredFolders.has(firstFolder(rel));
}

function asArray(value) {
  if (value == null) {
    return [];
  }
  return Array.isArray(value) ? value : [value];
}

function loadVaultGraph(options = {}) {
  const vaultRoot = getVaultRoot(options);
  const markdownFiles = walk(vaultRoot, (filePath) => filePath.endsWith('.md'));
  const baseFiles = walk(vaultRoot, (filePath) => filePath.endsWith('.base'));
  const index = buildNoteIndex([...markdownFiles, ...baseFiles], vaultRoot);
  const notes = markdownFiles.map((filePath) => parseMarkdown(filePath, vaultRoot));
  const frontmatterByRel = buildFrontmatterByRel(notes);
  const noteByRel = new Map(notes.map((note) => [note.rel, note]));
  return { vaultRoot, markdownFiles, baseFiles, index, notes, frontmatterByRel, noteByRel };
}

function getNoteTitle(noteOrFrontmatter, rel = '') {
  const frontmatter = noteOrFrontmatter?.frontmatter || noteOrFrontmatter || {};
  return frontmatter.title || path.basename(rel || noteOrFrontmatter?.rel || '', '.md');
}

function wikilinkAlias(value) {
  return String(value || '')
    .replace(/[\[\]|]/g, ' ')
    .replace(/[\r\n]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
}

function linkForRel(rel, title) {
  const target = noteKeyForRel(rel);
  const alias = wikilinkAlias(title) || path.basename(target);
  return `[[${target}|${alias}]]`;
}

function normalizeInput(value) {
  return String(value || '')
    .toLowerCase()
    .replace(/\.md\b/g, '')
    .replace(/[^a-z0-9]+/g, ' ')
    .trim();
}

function inputContainsAlias(input, alias) {
  const normalizedInput = ` ${normalizeInput(input)} `;
  const aliasTokens = normalizeInput(alias).split(' ').filter(Boolean);
  return aliasTokens.length > 0 && aliasTokens.every((token) => normalizedInput.includes(` ${token} `));
}

function findRouteDefinition(input, definitions = getRouteDefinitions()) {
  const normalizedInput = normalizeInput(input);
  for (const definition of definitions) {
    if (!isUsableRouteDefinition(definition)) {
      continue;
    }
    const processName = normalizeInput(path.basename(definition.processRel, '.md'));
    const processPath = normalizeInput(noteKeyForRel(definition.processRel));
    if (
      normalizedInput === normalizeInput(definition.id)
      || normalizedInput === processName
      || normalizedInput === processPath
    ) {
      return definition;
    }
  }
  return definitions.find((definition) =>
    isUsableRouteDefinition(definition)
    && routeAliases(definition).some((alias) => inputContainsAlias(input, alias))
  ) || null;
}

function resolveNoteInput(input, graph, expectedType = null) {
  if (!input) {
    return null;
  }
  const direct = resolveTarget(input, graph.index);
  if (direct) {
    const note = graph.noteByRel.get(direct);
    if (!expectedType || note?.frontmatter?.type === expectedType) {
      return direct;
    }
  }
  const normalizedInput = normalizeInput(input);
  for (const note of graph.notes) {
    if (expectedType && note.frontmatter?.type !== expectedType) {
      continue;
    }
    const title = normalizeInput(getNoteTitle(note, note.rel));
    const basename = normalizeInput(path.basename(note.rel, '.md'));
    if (normalizedInput === title || normalizedInput === basename) {
      return note.rel;
    }
  }
  return null;
}

function resolveRelationshipLinks(values, graph) {
  return asArray(values)
    .flatMap((value) => typeof value === 'string' ? extractWikilinkTargets(value) : [])
    .map((target) => resolveTarget(target, graph.index))
    .filter(Boolean);
}

function buildRoute(input, options = {}) {
  const env = options.env || process.env;
  const graph = options.graph || loadVaultGraph(options);
  const definitions = options.routeDefinitions || getRouteDefinitions(env);
  const definition = findRouteDefinition(input, definitions);
  const processRel = definition
    ? resolveNoteInput(definition.processRel, graph, 'process')
    : resolveNoteInput(input, graph, 'process');
  if (definition && !processRel) {
    return {
      graph,
      definition,
      processRel: null,
      error: `Route "${definition.id}" points to missing or non-process note ${definition.processRel}`
    };
  }
  if (!processRel) {
    return { graph, definition: null, processRel: null, error: `No notes route matched "${input}"` };
  }
  const processNote = graph.noteByRel.get(processRel);
  if (!processNote?.frontmatter) {
    return { graph, definition, processRel, error: `${processRel} is missing process frontmatter` };
  }
  const frontmatter = processNote.frontmatter;
  const runbookRels = options.runbook
    ? [resolveNoteInput(options.runbook, graph, 'runbook')].filter(Boolean)
    : resolveRelationshipLinks(frontmatter.related_runbooks, graph);
  const decisionRels = resolveRelationshipLinks(frontmatter.related_decisions, graph);
  const evidenceRels = resolveRelationshipLinks(frontmatter.related_evidence, graph);
  return {
    graph,
    definition,
    processRel,
    processNote,
    runbookRels,
    decisionRels,
    evidenceRels,
    error: null
  };
}

function currentDateParts(date = new Date()) {
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, '0');
  const day = String(date.getDate()).padStart(2, '0');
  const hours = String(date.getHours()).padStart(2, '0');
  const minutes = String(date.getMinutes()).padStart(2, '0');
  const timeZoneName = new Intl.DateTimeFormat('en-US', { timeZoneName: 'short' })
    .formatToParts(date)
    .find((part) => part.type === 'timeZoneName')?.value || 'local';
  return { date: `${year}-${month}-${day}`, time: `${hours}:${minutes}`, timeZoneName };
}

function dumpFrontmatter(frontmatter) {
  return yaml.dump(frontmatter, {
    lineWidth: 120,
    noRefs: true,
    quotingType: '"',
    forceQuotes: false
  }).trimEnd();
}

function splitFrontmatter(text) {
  if (!text.startsWith('---\n')) {
    return { frontmatter: null, body: text, rawFrontmatter: null };
  }
  const endIndex = text.indexOf('\n---\n', 4);
  if (endIndex === -1) {
    return { frontmatter: null, body: text, rawFrontmatter: null };
  }
  return {
    rawFrontmatter: text.slice(4, endIndex),
    frontmatter: yaml.load(text.slice(4, endIndex)) || {},
    body: text.slice(endIndex + 5)
  };
}

module.exports = {
  allowedTypes,
  allowedStatuses,
  allowedConfidence,
  relationshipTypeExpectations,
  get routeDefinitions() {
    return getRouteDefinitions();
  },
  defaultRouteDefinitions,
  getConfig,
  getRouteDefinitions,
  structuredFolders,
  getRepoRoot,
  getVaultRoot,
  toPosix,
  relativePath,
  walk,
  parseMarkdown,
  noteKeyForRel,
  buildNoteIndex,
  buildFrontmatterByRel,
  extractWikilinkTargets,
  validateRouteDefinitions,
  validateRouteConfig,
  findMalformedWikilinks,
  resolveTarget,
  firstFolder,
  isTemplate,
  isDaily,
  isStructured,
  asArray,
  loadVaultGraph,
  getNoteTitle,
  wikilinkAlias,
  linkForRel,
  normalizeInput,
  findRouteDefinition,
  resolveNoteInput,
  resolveRelationshipLinks,
  buildRoute,
  currentDateParts,
  dumpFrontmatter,
  splitFrontmatter
};
