const fs = require('node:fs');
const path = require('node:path');
const yaml = require('js-yaml');

const defaultRepoRoot = path.resolve(__dirname, '..', '..');
const frontmatterSchema = new yaml.Schema({
  implicit: yaml.DEFAULT_SCHEMA.compiledImplicit.filter(
    (type) => type.tag !== 'tag:yaml.org,2002:timestamp'
  ),
  explicit: yaml.DEFAULT_SCHEMA.compiledExplicit
});

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
  'Dashboards',
  'Processes',
  'Runbooks',
  'Decisions',
  'Incidents',
  'Evidence',
  'Releases',
  'Templates',
  'Known-Good'
]);

const defaultRouteDefinitions = [
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
    const frontmatter = loadFrontmatter(rawFrontmatter);
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

function loadFrontmatter(rawFrontmatter) {
  return yaml.load(rawFrontmatter, { schema: frontmatterSchema }) || {};
}

function normalizeFrontmatterDateFields(frontmatter) {
  const normalized = { ...frontmatter };
  for (const field of ['date', 'last_verified']) {
    if (!(normalized[field] instanceof Date)) {
      continue;
    }
    if (Number.isNaN(normalized[field].getTime())) {
      throw new Error(`Frontmatter ${field} is an invalid Date`);
    }
    normalized[field] = normalized[field].toISOString().slice(0, 10);
  }
  return normalized;
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
    const basenameKey = basename.toLowerCase();
    const candidates = byBasename.get(basenameKey) || [];
    candidates.push(rel);
    byBasename.set(basenameKey, candidates);
  }
  for (const candidates of byBasename.values()) {
    candidates.sort();
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

function markdownLinesOutsideFences(text) {
  const source = String(text || '');
  const lines = source.split('\n');
  const outside = [];
  let fence = null;
  let offset = 0;
  for (const rawLine of lines) {
    const line = rawLine.endsWith('\r') ? rawLine.slice(0, -1) : rawLine;
    if (fence) {
      const closePattern = new RegExp(
        `^ {0,3}\\${fence.character}{${fence.length},}[ \\t]*$`
      );
      if (closePattern.test(line)) {
        fence = null;
      }
      offset += rawLine.length + 1;
      continue;
    }
    const fenceMatch = line.match(/^ {0,3}(`{3,}|~{3,})(.*)$/);
    if (fenceMatch) {
      const marker = fenceMatch[1];
      fence = { character: marker[0], length: marker.length };
      offset += rawLine.length + 1;
      continue;
    }
    outside.push({ line, start: offset });
    offset += rawLine.length + 1;
  }
  return outside;
}

function extractWikilinkTargets(text) {
  const targets = [];
  for (const { line } of markdownLinesOutsideFences(text)) {
    const pattern = /!?\[\[([^\]\n]+)\]\]/g;
    let match;
    while ((match = pattern.exec(line))) {
      if (match[1].includes('[') || match[1].includes(']')) {
        continue;
      }
      const withoutAlias = match[1].split('|')[0].trim();
      const withoutHeading = withoutAlias.split('#')[0].trim();
      if (withoutHeading) {
        targets.push(withoutHeading);
      }
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
  const aliasOwners = new Map();

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
          return;
        }
        const normalizedAlias = normalizeInput(alias);
        if (!normalizedAlias) {
          errors.push(`${label}: aliases[${aliasIndex}] must contain letters or numbers`);
          return;
        }
        const owners = aliasOwners.get(normalizedAlias) || [];
        owners.push(label);
        aliasOwners.set(normalizedAlias, owners);
      });
    }

    if (!graph || typeof definition.processRel !== 'string' || !definition.processRel.trim()) {
      return;
    }
    const resolution = resolveTargetDetailed(definition.processRel, graph.index);
    if (resolution.status !== 'resolved') {
      if (requireExistingProcessTargets) {
        if (resolution.status === 'ambiguous') {
          errors.push(
            `${label}: processRel ${definition.processRel} is ambiguous; matches ${resolution.candidates.join(', ')}`
          );
        } else {
          errors.push(`${label}: processRel ${definition.processRel} must target an existing process note`);
        }
      }
      return;
    }
    const resolved = resolution.rel;
    if (isTemplate(resolved)) {
      errors.push(
        `${label}: processRel ${definition.processRel} must target a non-template process note; found template ${resolved}; instantiate the template with notes:new instead`
      );
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

  for (const [alias, owners] of [...aliasOwners.entries()].sort(([left], [right]) => left.localeCompare(right))) {
    if (owners.length > 1) {
      errors.push(
        `route aliases normalize to duplicate "${alias}": ${[...owners].sort().join(', ')}`
      );
    }
  }

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
  const malformed = [];
  for (const { line } of markdownLinesOutsideFences(text)) {
    let index = 0;
    while ((index = line.indexOf('[[', index)) !== -1) {
      const closeIndex = line.indexOf(']]', index + 2);
      if (closeIndex === -1) {
        malformed.push(line.slice(index).trim());
        index += 2;
        continue;
      }

      const inner = line.slice(index + 2, closeIndex);
      if (!inner.trim() || inner.includes('[') || inner.includes(']')) {
        malformed.push(line.slice(index, closeIndex + 2));
      }
      index = closeIndex + 2;
    }
  }

  return malformed;
}

function resolveTargetDetailed(target, index) {
  if (typeof target !== 'string' || !target.trim()) {
    return { status: 'missing', candidates: [] };
  }
  const normalized = target.trim().replace(/\.(md|base)$/i, '').replace(/\\/g, '/').toLowerCase();
  if (normalized.includes('/')) {
    if (index.byPath.has(normalized)) {
      const rel = index.byPath.get(normalized);
      return { status: 'resolved', rel, via: 'path', candidates: [rel] };
    }
    return { status: 'missing', candidates: [] };
  }
  const basename = path.basename(normalized);
  const candidates = index.byBasename.get(basename) || [];
  if (candidates.length === 1) {
    return {
      status: 'resolved',
      rel: candidates[0],
      via: 'basename',
      candidates: [...candidates]
    };
  }
  if (candidates.length > 1) {
    return { status: 'ambiguous', candidates: [...candidates] };
  }
  return { status: 'missing', candidates: [] };
}

function resolveTarget(target, index) {
  const resolution = resolveTargetDetailed(target, index);
  return resolution.status === 'resolved' ? resolution.rel : null;
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

function sortRouteCandidates(candidates) {
  return [...candidates].sort((left, right) => {
    const idComparison = normalizeInput(left.definition.id)
      .localeCompare(normalizeInput(right.definition.id));
    if (idComparison !== 0) {
      return idComparison;
    }
    const pathComparison = String(left.definition.processRel)
      .localeCompare(String(right.definition.processRel));
    return pathComparison !== 0 ? pathComparison : left.index - right.index;
  });
}

function routeMatchResult(matches, via) {
  const sorted = sortRouteCandidates(matches);
  if (sorted.length === 0) {
    return { status: 'missing', candidates: [] };
  }
  if (sorted.length === 1) {
    return {
      status: 'resolved',
      definition: sorted[0].definition,
      via,
      candidates: [sorted[0].definition]
    };
  }
  return {
    status: 'ambiguous',
    via,
    candidates: sorted.map((match) => match.definition)
  };
}

function findRouteDefinitionDetailed(input, definitions = getRouteDefinitions()) {
  const normalizedInput = normalizeInput(input);
  const usable = definitions
    .map((definition, index) => ({ definition, index }))
    .filter(({ definition }) => isUsableRouteDefinition(definition));
  const tiers = [
    {
      via: 'id',
      matches: usable.filter(({ definition }) =>
        normalizedInput === normalizeInput(definition.id)
      )
    },
    {
      via: 'process-path',
      matches: usable.filter(({ definition }) =>
        normalizedInput === normalizeInput(noteKeyForRel(definition.processRel))
      )
    },
    {
      via: 'process-name',
      matches: usable.filter(({ definition }) =>
        normalizedInput === normalizeInput(path.basename(definition.processRel, '.md'))
      )
    },
    {
      via: 'alias',
      matches: usable.filter(({ definition }) =>
        routeAliases(definition).some((alias) => inputContainsAlias(input, alias))
      )
    }
  ];
  for (const tier of tiers) {
    if (tier.matches.length > 0) {
      return routeMatchResult(tier.matches, tier.via);
    }
  }
  return { status: 'missing', candidates: [] };
}

function findRouteDefinition(input, definitions = getRouteDefinitions()) {
  const result = findRouteDefinitionDetailed(input, definitions);
  return result.status === 'resolved' ? result.definition : null;
}

function resolveNoteInput(input, graph, expectedType = null) {
  if (!input) {
    return null;
  }
  const direct = resolveTarget(input, graph.index);
  if (direct) {
    const note = graph.noteByRel.get(direct);
    if (
      !isTemplate(direct)
      && (!expectedType || note?.frontmatter?.type === expectedType)
    ) {
      return direct;
    }
  }
  const normalizedInput = normalizeInput(input);
  const matches = [];
  for (const note of graph.notes) {
    if (isTemplate(note.rel)) {
      continue;
    }
    if (expectedType && note.frontmatter?.type !== expectedType) {
      continue;
    }
    const title = normalizeInput(getNoteTitle(note, note.rel));
    const basename = normalizeInput(path.basename(note.rel, '.md'));
    if (normalizedInput === title || normalizedInput === basename) {
      matches.push(note.rel);
    }
  }
  const uniqueMatches = [...new Set(matches)].sort();
  return uniqueMatches.length === 1 ? uniqueMatches[0] : null;
}

function resolveRelationshipLinks(values, graph, expectedTypes = null) {
  const allowedTypes = expectedTypes == null
    ? null
    : expectedTypes instanceof Set
      ? expectedTypes
      : new Set(asArray(expectedTypes));
  return asArray(values)
    .flatMap((value) => typeof value === 'string' ? extractWikilinkTargets(value) : [])
    .map((target) => resolveTarget(target, graph.index))
    .filter((rel) => {
      if (!rel || isTemplate(rel)) {
        return false;
      }
      if (!allowedTypes) {
        return true;
      }
      const targetFrontmatter = graph.frontmatterByRel?.get(rel)
        || graph.noteByRel?.get(rel)?.frontmatter;
      return allowedTypes.has(targetFrontmatter?.type);
    });
}

function buildRoute(input, options = {}) {
  const env = options.env || process.env;
  const graph = options.graph || loadVaultGraph(options);
  const definitions = options.routeDefinitions || getRouteDefinitions(env);
  const routeMatch = findRouteDefinitionDetailed(input, definitions);
  if (routeMatch.status === 'ambiguous') {
    const candidates = routeMatch.candidates.map((candidate) =>
      `route "${candidate.id}" -> ${candidate.processRel}`
    );
    return {
      graph,
      definition: null,
      processRel: null,
      error: `Ambiguous notes route for "${input}": ${candidates.join(', ')}; use an exact route id or process path`
    };
  }
  const definition = routeMatch.status === 'resolved' ? routeMatch.definition : null;
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
    : resolveRelationshipLinks(
      frontmatter.related_runbooks,
      graph,
      relationshipTypeExpectations.related_runbooks
    );
  const decisionRels = resolveRelationshipLinks(
    frontmatter.related_decisions,
    graph,
    relationshipTypeExpectations.related_decisions
  );
  const evidenceRels = resolveRelationshipLinks(
    frontmatter.related_evidence,
    graph,
    relationshipTypeExpectations.related_evidence
  );
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
  return yaml.dump(normalizeFrontmatterDateFields(frontmatter), {
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
    frontmatter: loadFrontmatter(text.slice(4, endIndex)),
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
  loadFrontmatter,
  normalizeFrontmatterDateFields,
  parseMarkdown,
  noteKeyForRel,
  buildNoteIndex,
  buildFrontmatterByRel,
  markdownLinesOutsideFences,
  extractWikilinkTargets,
  validateRouteDefinitions,
  validateRouteConfig,
  findMalformedWikilinks,
  resolveTargetDetailed,
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
  findRouteDefinitionDetailed,
  findRouteDefinition,
  resolveNoteInput,
  resolveRelationshipLinks,
  buildRoute,
  currentDateParts,
  dumpFrontmatter,
  splitFrontmatter
};
