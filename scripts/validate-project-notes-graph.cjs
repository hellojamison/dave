#!/usr/bin/env node

const {
  validateProjectNotesGraph
} = require('./lib/validate-project-notes-graph.cjs');

const summarizedWarningCategories = [
  {
    label: 'typed notes without related_apps',
    matches: (warning) => warning.endsWith(': typed note has no related_apps')
  },
  {
    label: 'legacy daily notes without frontmatter',
    matches: (warning) => warning.endsWith(': legacy daily note has no frontmatter')
  },
  {
    label: 'other legacy notes without frontmatter',
    matches: (warning) => warning.endsWith(': legacy note has no frontmatter')
  },
  {
    label: 'structured notes without type',
    matches: (warning) => warning.endsWith(': structured note is missing type')
  },
  {
    label: 'legacy structured notes without frontmatter',
    matches: (warning) => warning.endsWith(': legacy structured note is missing frontmatter')
  }
];

function printWarnings(allWarnings, verboseWarnings = false) {
  if (verboseWarnings) {
    for (const warning of allWarnings) {
      console.warn(`WARN ${warning}`);
    }
    return;
  }

  const summaryCounts = new Map(
    summarizedWarningCategories.map(({ label }) => [label, 0])
  );
  const actionableWarnings = [];

  for (const warning of allWarnings) {
    const category = summarizedWarningCategories.find(({ matches }) => matches(warning));
    if (category) {
      summaryCounts.set(category.label, summaryCounts.get(category.label) + 1);
    } else {
      actionableWarnings.push(warning);
    }
  }

  for (const warning of actionableWarnings) {
    console.warn(`WARN ${warning}`);
  }

  const summarizedCount = [...summaryCounts.values()].reduce((sum, count) => sum + count, 0);
  if (summarizedCount > 0) {
    console.warn(`WARN Summarized ${summarizedCount} recurring warning(s):`);
    for (const { label } of summarizedWarningCategories) {
      const count = summaryCounts.get(label);
      if (count > 0) {
        console.warn(`WARN   ${count} ${label}`);
      }
    }
    console.warn('WARN Re-run with --verbose to print every warning.');
  }
}

function main(args = process.argv.slice(2)) {
  const supportedArgs = new Set(['--verbose']);
  const unknownArgs = args.filter((arg) => !supportedArgs.has(arg));
  const verboseWarnings = args.includes('--verbose');

  if (unknownArgs.length > 0) {
    console.error(`ERROR Unknown option(s): ${unknownArgs.join(', ')}. Supported option: --verbose`);
    return 2;
  }

  const { errors, warnings } = validateProjectNotesGraph();
  printWarnings(warnings, verboseWarnings);

  if (errors.length > 0) {
    for (const error of errors) {
      console.error(`ERROR ${error}`);
    }
    return 1;
  }

  console.log(`Project notes graph validation passed with ${warnings.length} warning(s).`);
  return 0;
}

if (require.main === module) {
  process.exitCode = main();
}

module.exports = {
  main,
  printWarnings,
  summarizedWarningCategories
};
