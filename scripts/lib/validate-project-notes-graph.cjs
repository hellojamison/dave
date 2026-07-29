const fs = require('node:fs');
const path = require('node:path');
const yaml = require('js-yaml');
const {
  allowedTypes,
  allowedStatuses,
  allowedConfidence,
  relationshipTypeExpectations,
  defaultRouteDefinitions,
  getConfig,
  getVaultRoot,
  loadVaultGraph,
  loadFrontmatter,
  buildNoteIndex,
  buildFrontmatterByRel,
  extractWikilinkTargets,
  validateRouteConfig,
  findMalformedWikilinks,
  resolveTargetDetailed,
  resolveTarget,
  asArray,
  isTemplate,
  isDaily,
  isStructured
} = require('./project-notes-graph.cjs');

const allowedBaseViewTypes = new Set(['table', 'cards', 'list', 'map']);

function compareStrings(left, right) {
  if (left < right) {
    return -1;
  }
  if (left > right) {
    return 1;
  }
  return 0;
}

function stableSort(values) {
  return [...values].sort(compareStrings);
}

function hasInbound(inboundByRel, rel) {
  return (inboundByRel.get(rel) || new Set()).size > 0;
}

function wikilinkResolutionError(rel, target, resolution, context = '') {
  const prefix = context ? `${context} ` : '';
  if (resolution.status === 'ambiguous') {
    return `${rel}: ${prefix}has ambiguous wikilink [[${target}]] matching ${resolution.candidates.join(', ')}; use a vault-relative path`;
  }
  return `${rel}: ${prefix}has broken wikilink [[${target}]]`;
}

function daysSince(value, now = new Date()) {
  if (!value) {
    return null;
  }
  const date = value instanceof Date
    ? value
    : new Date(/^\d{4}-\d{2}-\d{2}$/.test(value) ? `${value}T00:00:00Z` : value);
  if (Number.isNaN(date.getTime())) {
    return null;
  }
  return Math.floor((now.getTime() - date.getTime()) / 86400000);
}

function isNonEmptyString(value) {
  return typeof value === 'string' && value.trim() !== '';
}

function isPresentTitle(value) {
  return isNonEmptyString(value);
}

function isDateOnly(value) {
  if (typeof value !== 'string' || !/^\d{4}-\d{2}-\d{2}$/.test(value)) {
    return false;
  }
  const [year, month, day] = value.split('-').map(Number);
  const parsed = new Date(Date.UTC(year, month - 1, day));
  return parsed.getUTCFullYear() === year
    && parsed.getUTCMonth() === month - 1
    && parsed.getUTCDate() === day;
}

function validateSchemaManagedFrontmatter(rel, frontmatter) {
  const fieldErrors = [];
  if (!isPresentTitle(frontmatter.title)) {
    fieldErrors.push(`${rel}: schema-managed note is missing title`);
  }
  for (const field of ['type', 'status']) {
    if (!isNonEmptyString(frontmatter[field])) {
      fieldErrors.push(`${rel}: schema-managed note is missing ${field}`);
    }
  }
  if (!isDateOnly(frontmatter.date)) {
    fieldErrors.push(`${rel}: schema-managed note date must be YYYY-MM-DD`);
  }
  if (frontmatter.last_verified != null && !isDateOnly(frontmatter.last_verified)) {
    fieldErrors.push(`${rel}: schema-managed note last_verified must be YYYY-MM-DD`);
  }
  if (
    !Array.isArray(frontmatter.tags)
    || frontmatter.tags.length === 0
    || frontmatter.tags.some((tag) => !isNonEmptyString(tag))
  ) {
    fieldErrors.push(`${rel}: schema-managed note tags must be a non-empty array of strings`);
  }
  return fieldErrors;
}

function isPlainObject(value) {
  return value != null && typeof value === 'object' && !Array.isArray(value);
}

function formulaNameFromProperty(value) {
  if (typeof value !== 'string' || !value.startsWith('formula.')) {
    return null;
  }
  return value.slice('formula.'.length);
}

function validateBaseSchema(rel, base) {
  if (!isPlainObject(base)) {
    return [`${rel}: Base YAML must be an object`];
  }

  const baseErrors = [];
  const formulas = isPlainObject(base.formulas) ? base.formulas : {};
  const formulaNames = new Set(Object.keys(formulas));

  if (base.properties != null) {
    if (!isPlainObject(base.properties)) {
      baseErrors.push(`${rel}: properties must be an object`);
    } else {
      for (const key of Object.keys(base.properties)) {
        const formulaName = formulaNameFromProperty(key);
        if (formulaName && !formulaNames.has(formulaName)) {
          baseErrors.push(`${rel}: properties references undefined formula.${formulaName}`);
        }
      }
    }
  }

  if (!Array.isArray(base.views) || base.views.length === 0) {
    baseErrors.push(`${rel}: views must be a non-empty array`);
    return baseErrors;
  }

  base.views.forEach((view, index) => {
    const label = `${rel}: views[${index}]`;
    if (!isPlainObject(view)) {
      baseErrors.push(`${label} must be an object`);
      return;
    }

    if (!allowedBaseViewTypes.has(view.type)) {
      baseErrors.push(`${label}.type must be one of table, cards, list, or map`);
    }
    if (!isNonEmptyString(view.name)) {
      baseErrors.push(`${label}.name must be a non-empty string`);
    }
    if (view.order != null) {
      if (!Array.isArray(view.order)) {
        baseErrors.push(`${label}.order must be an array`);
      } else {
        view.order.forEach((entry, orderIndex) => {
          const formulaName = formulaNameFromProperty(entry);
          if (formulaName && !formulaNames.has(formulaName)) {
            baseErrors.push(`${label}.order[${orderIndex}] references undefined formula.${formulaName}`);
          }
        });
      }
    }
  });

  return baseErrors;
}

function normalizeVirtualRel(value) {
  const rel = String(value).replace(/\\/g, '/').replace(/^\.\/+/, '');
  const normalized = path.posix.normalize(rel);
  if (
    !normalized
    || normalized === '.'
    || normalized === '..'
    || normalized.startsWith('../')
    || path.posix.isAbsolute(normalized)
  ) {
    throw new Error(`Invalid virtual vault path: ${value}`);
  }
  return normalized;
}

function virtualFileEntries(files) {
  if (files instanceof Map) {
    return [...files.entries()];
  }
  if (files && typeof files === 'object' && !Array.isArray(files)) {
    return Object.entries(files);
  }
  throw new TypeError('files must be a Map or an object keyed by vault-relative path');
}

function parseVirtualMarkdown(rel, text, vaultRoot) {
  const filePath = path.join(vaultRoot, ...rel.split('/'));
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
    return {
      rel,
      filePath,
      text,
      frontmatter: loadFrontmatter(rawFrontmatter),
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

function createValidationGraphFromFiles(files, options = {}) {
  const vaultRoot = path.resolve(options.vaultRoot || '/virtual-project-notes');
  const entries = virtualFileEntries(files)
    .map(([rel, contents]) => [
      normalizeVirtualRel(rel),
      Buffer.isBuffer(contents) ? contents.toString('utf8') : String(contents)
    ])
    .sort(([left], [right]) => compareStrings(left, right));
  const markdownEntries = entries.filter(([rel]) => rel.endsWith('.md'));
  const baseEntries = entries.filter(([rel]) => rel.endsWith('.base'));
  const allPaths = [...markdownEntries, ...baseEntries]
    .map(([rel]) => path.join(vaultRoot, ...rel.split('/')));
  const markdownFiles = markdownEntries.map(([rel]) => path.join(vaultRoot, ...rel.split('/')));
  const baseFiles = baseEntries.map(([rel]) => path.join(vaultRoot, ...rel.split('/')));
  const notes = markdownEntries.map(([rel, text]) => parseVirtualMarkdown(rel, text, vaultRoot));
  const frontmatterByRel = buildFrontmatterByRel(notes);
  return {
    vaultRoot,
    markdownFiles,
    baseFiles,
    baseContentsByRel: new Map(baseEntries),
    index: buildNoteIndex(allPaths, vaultRoot),
    notes,
    frontmatterByRel,
    noteByRel: new Map(notes.map((note) => [note.rel, note]))
  };
}

function validateBaseFiles(graph, vaultRoot, readFileSync) {
  const errors = [];
  const baseContentsByRel = graph.baseContentsByRel instanceof Map
    ? graph.baseContentsByRel
    : null;

  if (baseContentsByRel) {
    for (const [rel, text] of stableSort([...baseContentsByRel.keys()])
      .map((rel) => [rel, baseContentsByRel.get(rel)])) {
      try {
        errors.push(...validateBaseSchema(rel, yaml.load(text)));
      } catch (error) {
        errors.push(`${rel}: invalid Base YAML: ${error.message}`);
      }
    }
    return errors;
  }

  for (const baseFile of stableSort(graph.baseFiles || [])) {
    const rel = path.relative(vaultRoot, baseFile).split(path.sep).join('/');
    try {
      const base = yaml.load(readFileSync(baseFile, 'utf8'));
      errors.push(...validateBaseSchema(rel, base));
    } catch (error) {
      errors.push(`${rel}: invalid Base YAML: ${error.message}`);
    }
  }
  return errors;
}

function validateProjectNotesGraph(options = {}) {
  const env = options.env || process.env;
  const suppliedGraph = options.graph || null;
  const vaultRoot = path.resolve(
    options.vaultRoot
      || suppliedGraph?.vaultRoot
      || getVaultRoot({ env })
  );
  const errors = [];
  const warnings = [];
  const existsSync = options.existsSync || fs.existsSync;
  const readFileSync = options.readFileSync || fs.readFileSync;
  const now = options.now || new Date();

  let graph = suppliedGraph;
  if (!graph && options.files != null) {
    graph = createValidationGraphFromFiles(options.files, { vaultRoot });
  }

  if (!graph && !existsSync(vaultRoot)) {
    return {
      vaultRoot,
      errors: [`Missing vault root: ${vaultRoot}`],
      warnings: []
    };
  }

  graph ||= loadVaultGraph({ vaultRoot, env });
  const { notes, index, frontmatterByRel } = graph;
  const inboundByRel = new Map();
  const config = options.config !== undefined
    ? options.config
    : options.files != null
      ? {}
      : getConfig(env);
  errors.push(...validateRouteConfig(config, graph));

  for (const note of notes) {
    const inboundTargets = extractWikilinkTargets(note.body);
    if (note.frontmatter) {
      for (const field of Object.keys(relationshipTypeExpectations)) {
        for (const value of asArray(note.frontmatter[field])) {
          if (typeof value === 'string') {
            inboundTargets.push(...extractWikilinkTargets(value));
          }
        }
      }
    }
    for (const target of inboundTargets) {
      const resolved = resolveTarget(target, index);
      if (resolved && resolved !== note.rel) {
        if (!inboundByRel.has(resolved)) {
          inboundByRel.set(resolved, new Set());
        }
        inboundByRel.get(resolved).add(note.rel);
      }
    }
  }

  const appNoteNames = new Set(
    notes
      .filter((note) => note.rel.startsWith('Apps/') && note.frontmatter?.type === 'app')
      .flatMap((note) => [
        path.basename(note.rel, '.md'),
        note.frontmatter.title
      ].filter(Boolean))
  );

  if (config.routes == null || (Array.isArray(config.routes) && config.routes.length === 0)) {
    for (const definition of defaultRouteDefinitions) {
      if (typeof definition?.processRel !== 'string' || !definition.processRel.trim()) {
        continue;
      }
      if (!resolveTarget(definition.processRel, index)) {
        warnings.push(`route alias "${definition.id}" points to missing ${definition.processRel}`);
      }
    }
  }

  for (const note of notes) {
    const { rel, frontmatter, text, body, frontmatterError } = note;
    const structured = isStructured(rel);
    const template = isTemplate(rel);
    const schemaVersion = frontmatter?.schema_version;
    const schemaManaged = schemaVersion === 1 || schemaVersion === '1';

    for (const malformed of findMalformedWikilinks(text)) {
      errors.push(`${rel}: malformed wikilink ${malformed}`);
    }

    if (frontmatterError) {
      errors.push(`${rel}: ${frontmatterError}`);
    }

    if (structured || isDaily(rel) || frontmatter?.type === 'daily' || schemaManaged) {
      const linkTargets = extractWikilinkTargets(body);
      for (const target of linkTargets) {
        const resolution = resolveTargetDetailed(target, index);
        if (resolution.status !== 'resolved') {
          errors.push(wikilinkResolutionError(rel, target, resolution));
        }
      }
    }

    if (!frontmatter) {
      if (structured) {
        warnings.push(`${rel}: legacy structured note is missing frontmatter`);
      } else if (isDaily(rel)) {
        warnings.push(`${rel}: legacy daily note has no frontmatter`);
      } else {
        warnings.push(`${rel}: legacy note has no frontmatter`);
      }
      continue;
    }

    if (schemaManaged) {
      errors.push(...validateSchemaManagedFrontmatter(rel, frontmatter));
    }
    const draft = frontmatter.status === 'draft';

    if (frontmatter.type && !allowedTypes.has(frontmatter.type)) {
      const message = `${rel}: invalid type "${frontmatter.type}"`;
      if (schemaManaged) {
        errors.push(message);
      } else {
        warnings.push(message);
      }
    } else if (structured && !template && !frontmatter.type) {
      const message = `${rel}: structured note is missing type`;
      if (schemaManaged) {
        errors.push(message);
      } else {
        warnings.push(message);
      }
    } else if (!structured && !template && frontmatter.type && !frontmatter.status) {
      errors.push(`${rel}: promoted note is missing status`);
    }

    if (frontmatter.status && !allowedStatuses.has(frontmatter.status)) {
      const message = `${rel}: invalid status "${frontmatter.status}"`;
      if (schemaManaged) {
        errors.push(message);
      } else {
        warnings.push(message);
      }
    }

    if (frontmatter.confidence && !allowedConfidence.has(frontmatter.confidence)) {
      errors.push(`${rel}: invalid confidence "${frontmatter.confidence}"`);
    }

    if (!template && frontmatter.source_of_truth === true) {
      if (!frontmatter.last_verified) {
        errors.push(`${rel}: source_of_truth note is missing last_verified`);
      }
      if (!frontmatter.confidence) {
        errors.push(`${rel}: source_of_truth note is missing confidence`);
      }
      const age = daysSince(frontmatter.last_verified, now);
      if (age != null && age > 90) {
        warnings.push(`${rel}: source_of_truth last_verified is ${age} days old`);
      }
    }

    if (frontmatter.app && !appNoteNames.has(frontmatter.app)) {
      errors.push(`${rel}: app "${frontmatter.app}" has no matching app note`);
    }

    if (!template && frontmatter.type && asArray(frontmatter.related_apps).length === 0) {
      warnings.push(`${rel}: typed note has no related_apps`);
    }

    if (
      !template
      && frontmatter.type === 'process'
      && frontmatter.status !== 'archived'
      && !draft
    ) {
      if (asArray(frontmatter.related_runbooks).length === 0) {
        warnings.push(`${rel}: process note has no related_runbooks`);
      }
      if (asArray(frontmatter.related_decisions).length === 0) {
        warnings.push(`${rel}: process note has no related_decisions`);
      }
      if (asArray(frontmatter.related_evidence).length === 0) {
        warnings.push(`${rel}: process note has no related_evidence`);
      }
    }

    if (
      !template
      && frontmatter.created_by === 'project-notes-cli'
      && frontmatter.type === 'evidence'
    ) {
      if (asArray(frontmatter.related_apps).length === 0) {
        warnings.push(`${rel}: CLI-created evidence note has no related_apps`);
      }
      if (asArray(frontmatter.related_processes).length === 0) {
        warnings.push(`${rel}: CLI-created evidence note has no related_processes`);
      }
      if (asArray(frontmatter.related_runbooks).length === 0) {
        warnings.push(`${rel}: CLI-created evidence note has no related_runbooks`);
      }
    }

    const mustHaveInbound = !template
      && frontmatter.status !== 'archived'
      && !draft
      && (
        frontmatter.type === 'process'
        || frontmatter.type === 'runbook'
        || (frontmatter.type === 'decision' && frontmatter.source_of_truth === true)
      );
    if (mustHaveInbound && !hasInbound(inboundByRel, rel)) {
      warnings.push(`${rel}: ${frontmatter.type} note has no inbound links`);
    }

    for (const field of Object.keys(relationshipTypeExpectations)) {
      for (const value of asArray(frontmatter[field])) {
        if (typeof value === 'string') {
          for (const target of extractWikilinkTargets(value)) {
            const resolution = resolveTargetDetailed(target, index);
            if (resolution.status !== 'resolved') {
              errors.push(wikilinkResolutionError(rel, target, resolution, field));
              continue;
            }
            const resolved = resolution.rel;
            if (isTemplate(resolved)) {
              errors.push(
                `${rel}: ${field} target [[${target}]] is a template; instantiate it with notes:new before using it as a typed relationship`
              );
              continue;
            }
            const targetFrontmatter = frontmatterByRel.get(resolved);
            const expectedTypes = relationshipTypeExpectations[field];
            if (!targetFrontmatter?.type || !expectedTypes.has(targetFrontmatter.type)) {
              const expected = [...expectedTypes].join(' or ');
              const actual = targetFrontmatter?.type || 'missing type';
              errors.push(`${rel}: ${field} target [[${target}]] must be type ${expected}; found ${actual}`);
            }
          }
        }
      }
    }
  }

  errors.push(...validateBaseFiles(graph, vaultRoot, readFileSync));

  return {
    vaultRoot,
    errors: stableSort(errors),
    warnings: stableSort(warnings)
  };
}

module.exports = {
  createValidationGraphFromFiles,
  daysSince,
  isDateOnly,
  validateBaseSchema,
  validateProjectNotesGraph,
  validateSchemaManagedFrontmatter,
  wikilinkResolutionError
};
