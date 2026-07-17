#!/usr/bin/env node

const fs = require('node:fs');
const path = require('node:path');
const yaml = require('js-yaml');
const {
  allowedTypes,
  allowedStatuses,
  allowedConfidence,
  relationshipTypeExpectations,
  getConfig,
  getRouteDefinitions,
  getVaultRoot,
  loadVaultGraph,
  extractWikilinkTargets,
  validateRouteConfig,
  findMalformedWikilinks,
  resolveTarget,
  noteKeyForRel,
  asArray,
  isTemplate,
  isDaily,
  isStructured
} = require('./lib/project-notes-graph.cjs');

const vaultRoot = getVaultRoot();
const errors = [];
const warnings = [];
const allowedBaseViewTypes = new Set(['table', 'cards', 'list', 'map']);

function hasInbound(inboundByRel, rel) {
  return (inboundByRel.get(rel) || new Set()).size > 0;
}

function daysSince(value, now = new Date()) {
  if (!value) {
    return null;
  }
  const date = value instanceof Date ? value : new Date(`${value}T00:00:00`);
  if (Number.isNaN(date.getTime())) {
    return null;
  }
  return Math.floor((now.getTime() - date.getTime()) / 86400000);
}

function isNonEmptyString(value) {
  return typeof value === 'string' && value.trim() !== '';
}

function isPresentTitle(value) {
  return isNonEmptyString(value)
    || (value instanceof Date && !Number.isNaN(value.getTime()));
}

function isDateOnly(value) {
  if (typeof value === 'string') {
    return /^\d{4}-\d{2}-\d{2}$/.test(value);
  }
  return value instanceof Date
    && !Number.isNaN(value.getTime())
    && value.toISOString().endsWith('T00:00:00.000Z');
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

if (!fs.existsSync(vaultRoot)) {
  errors.push(`Missing vault root: ${vaultRoot}`);
} else {
  const graph = loadVaultGraph({ vaultRoot });
  const { notes, baseFiles, index, frontmatterByRel } = graph;
  const inboundByRel = new Map();
  const config = getConfig();
  errors.push(...validateRouteConfig(config, graph));

  for (const note of notes) {
    const inboundTargets = extractWikilinkTargets(note.text);
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
    for (const definition of getRouteDefinitions()) {
      if (typeof definition?.processRel !== 'string' || !definition.processRel.trim()) {
        continue;
      }
      if (!resolveTarget(definition.processRel, index)) {
        warnings.push(`route alias "${definition.id}" points to missing ${definition.processRel}`);
      }
    }
  }

  for (const note of notes) {
    const { rel, frontmatter, text, frontmatterError } = note;
    const structured = isStructured(rel);
    const template = isTemplate(rel);

    for (const malformed of findMalformedWikilinks(text)) {
      errors.push(`${rel}: malformed wikilink ${malformed}`);
    }

    if (frontmatterError) {
      errors.push(`${rel}: ${frontmatterError}`);
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
    const schemaVersion = frontmatter.schema_version;
    const schemaManaged = schemaVersion === 1 || schemaVersion === '1';

    if (schemaManaged) {
      errors.push(...validateSchemaManagedFrontmatter(rel, frontmatter));
    }

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
      const age = daysSince(frontmatter.last_verified);
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

    if (!template && frontmatter.type === 'process' && frontmatter.status !== 'archived') {
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

    if (!template && (structured || isDaily(rel) || frontmatter.type === 'daily')) {
      const linkTargets = extractWikilinkTargets(text);
      for (const target of linkTargets) {
        if (!resolveTarget(target, index)) {
          errors.push(`${rel}: broken wikilink [[${target}]]`);
        }
      }
    }

    const mustHaveInbound = !template
      && frontmatter.status !== 'archived'
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
            const resolved = resolveTarget(target, index);
            if (!resolved) {
              errors.push(`${rel}: ${field} has broken wikilink [[${target}]]`);
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

  for (const baseFile of baseFiles) {
    const rel = path.relative(vaultRoot, baseFile).split(path.sep).join('/');
    try {
      const base = yaml.load(fs.readFileSync(baseFile, 'utf8'));
      errors.push(...validateBaseSchema(rel, base));
    } catch (error) {
      errors.push(`${rel}: invalid Base YAML: ${error.message}`);
    }
  }
}

for (const warning of warnings) {
  console.warn(`WARN ${warning}`);
}

if (errors.length > 0) {
  for (const error of errors) {
    console.error(`ERROR ${error}`);
  }
  process.exit(1);
}

console.log(`Project notes graph validation passed with ${warnings.length} warning(s).`);
