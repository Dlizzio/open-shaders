'use strict';

const { execFileSync } = require('child_process');

// Field/record separators that won't appear in real commit text. Not NUL
// (\x00): that can't be passed as a process argument at all -- the OS spawn
// call rejects it since C strings are NUL-terminated.
const FIELD_SEP = '\x1f'; // ASCII unit separator
const RECORD_SEP = '\x1e'; // ASCII record separator

// Upstream sync PRs land as real 2-parent merge commits so future syncs stay
// possible via a plain `git merge upstream/dev` (see AGENTS.md's Upstream
// Sync Rules). But semantic-release's own getCommits() is a plain
// `git log <lastTag>..HEAD` with no --first-parent option, and that isn't
// something .releaserc.js can configure -- it's hardcoded in semantic-release
// core, not the plugins. Normally that's harmless (a sync merge only adds a
// handful of genuinely new upstream commits as the second parent). It stops
// being harmless the one time upstream rewrites its own history (as
// community-shaders did to purge proprietary ENB SDK headers, PR #537):
// every rewritten commit becomes a "new" second-parent ancestor, so a plain
// git log range picks up thousands of already-shipped commits as if they
// were new, corrupting both the changelog and the version-bump calculation
// (a stale `feat:` can force a minor bump when only a patch is warranted).
//
// This wraps the real commit-analyzer/release-notes-generator, recomputing
// context.commits with --first-parent before delegating to them. On a
// branch with no merge commits in range (every hotfix/X.Y.x line, and dev
// releases between syncs) --first-parent and the full graph are identical,
// so this is a no-op there -- it only changes anything right after a sync
// merge landed non-first-parent ancestry.
function getFirstParentCommits(cwd, from, to) {
  const range = from ? `${from}..${to}` : to;
  const format = `${RECORD_SEP}%H${FIELD_SEP}%B${FIELD_SEP}%d${FIELD_SEP}%ci`;
  const raw = execFileSync('git', ['log', '--first-parent', range, `--format=${format}`], {
    cwd,
    maxBuffer: 1024 * 1024 * 256,
    encoding: 'utf8',
  });
  return raw
    .split(RECORD_SEP)
    .filter((record) => record.length > 0)
    .map((record) => {
      const [hash, message, gitTags, committerDateStr] = record.split(FIELD_SEP);
      return {
        hash,
        message: (message || '').trim(),
        gitTags: (gitTags || '').trim(),
        committerDate: new Date(committerDateStr),
      };
    });
}

function firstParentContext(context) {
  const { cwd, lastRelease, nextRelease, commits, logger } = context;
  const from = lastRelease && lastRelease.gitHead;
  const to = (nextRelease && nextRelease.gitHead) || 'HEAD';
  const firstParentCommits = getFirstParentCommits(cwd, from, to);
  if (firstParentCommits.length !== commits.length) {
    logger.log(
      '[first-parent] %d commits via full history, %d via --first-parent; using --first-parent (a prior merge landed non-first-parent ancestry, e.g. an upstream history rewrite)',
      commits.length,
      firstParentCommits.length
    );
  }
  return { ...context, commits: firstParentCommits };
}

module.exports = {
  analyzeCommits: async (pluginConfig, context) => {
    const { analyzeCommits } = await import('@semantic-release/commit-analyzer');
    return analyzeCommits(pluginConfig, firstParentContext(context));
  },
  generateNotes: async (pluginConfig, context) => {
    const { generateNotes } = await import('@semantic-release/release-notes-generator');
    return generateNotes(pluginConfig, firstParentContext(context));
  },
};
