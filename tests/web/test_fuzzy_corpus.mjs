// Cross-client corpus oracle (algorithm), web side. Reads the SAME shared fixture as
// tests/test_fuzzy_corpus.cpp and asserts the web matcher (apps/web/fuzzy.mjs)
// reproduces the same match set and ordering per query as the C++ reference. The two
// clients sharing one fixture is what makes "identical results" enforceable rather
// than two independently-drifting corpora. Run: node tests/web/test_fuzzy_corpus.mjs

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { fuzzyRank } from '../../apps/web/fuzzy.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const fixture = join(here, '..', 'fixtures', 'fuzzy_corpus.tsv');

// The library-default matcher parameters. In the running client these arrive on the
// palette wire section; the corpus fixture was generated with the defaults, so the
// oracle scores with the same defaults. If these drift from the C++ constants the
// corpus orderings diverge and this test fails -- the intended cross-source guard.
const params = {
  baseScore: 10,
  wordBoundaryBonus: 8,
  contiguityBonus: 6,
  exactCaseBonus: 1,
  lengthCap: 100,
};

function loadCorpus() {
  const text = readFileSync(fixture, 'utf8');
  const candidates = [];
  const queries = [];
  for (const raw of text.split('\n')) {
    const line = raw.replace(/\r$/, '');
    if (line === '') continue;
    const f = line.split('\t');
    if (f[0] === 'C') {
      candidates.push({ id: f[1], label: f[2], detail: f[3] ?? '' });
    } else if (f[0] === 'Q') {
      const query = f[1] ?? '';
      const expected = (f[2] ?? '') === '' ? [] : f[2].split(',');
      queries.push({ query, expected });
    }
  }
  return { candidates, queries };
}

let failures = 0;
const { candidates, queries } = loadCorpus();
if (candidates.length === 0 || queries.length === 0) {
  console.error('  FAIL: empty corpus');
  failures++;
}
for (const { query, expected } of queries) {
  const order = fuzzyRank(candidates, query, params);
  const ids = order.map((i) => candidates[i].id);
  const got = ids.join(',');
  const want = expected.join(',');
  if (got !== want) {
    console.error(`  FAIL: query "${query}" -> [${got}] expected [${want}]`);
    failures++;
  }
}

if (failures > 0) {
  console.error(`fuzzy corpus oracle: ${failures} failure(s)`);
  process.exit(1);
}

// The published parameters arrive off the wire as BigInt (the web decoder yields
// BigInt for signed integers); the matcher must accept them and produce the same
// orderings as with Number params. This guards the wire-shape mismatch.
const bigParams = Object.fromEntries(
  Object.entries(params).map(([k, v]) => [k, BigInt(v)]),
);
for (const { query, expected } of queries) {
  const ids = fuzzyRank(candidates, query, bigParams).map((i) => candidates[i].id);
  if (ids.join(',') !== expected.join(',')) {
    console.error(`  FAIL: BigInt params query "${query}" -> [${ids.join(',')}]`);
    process.exit(1);
  }
}

console.log(`fuzzy corpus oracle: ${queries.length} queries matched the C++ reference`);

// An out-of-domain parameter is refused by the web matcher, mirroring the C++ decoder
// rejecting the frame -- a malformed frame cannot silently diverge the two clients.
try {
  fuzzyRank(candidates, 'save', { ...params, baseScore: 2_000_000 });
  console.error('  FAIL: expected out-of-domain parameters to be refused');
  process.exit(1);
} catch (e) {
  if (!(e instanceof RangeError)) {
    console.error('  FAIL: expected RangeError, got ' + e);
    process.exit(1);
  }
}
