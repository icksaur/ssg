// The shared fuzzy match-and-order contract, executed locally in the browser client
// for responsiveness. This mirrors the C++ reference (ssg::referenceRank in
// PaletteSearcher): a client narrows the library-published candidate universe by the
// user's live query with NO round trip, and MUST reproduce the library's match set
// and ordering byte-for-byte. Iteration and length are over raw UTF-8 BYTES (not
// UTF-16 code units); folding is ASCII-only. The scoring weights and length cap are
// NOT hardcoded here -- they arrive on the wire as the palette section's parameters
// and are passed in.

const enc = new TextEncoder();

// ASCII-only fold: A-Z -> a-z, every other byte (including UTF-8 continuation bytes)
// folds to itself. Deliberately not locale-aware, so this and the C++ reference agree.
function fold(byte) {
  return byte >= 0x41 && byte <= 0x5a ? byte + 0x20 : byte;
}

// Score `query` as a case-folded subsequence of `candidate` (both UTF-8 byte arrays);
// null when not a subsequence. Weights come from `p` (the published parameters).
function fuzzyScore(candidate, query, p) {
  if (query.length === 0) return 0;
  let score = 0;
  let cursor = 0;
  let previous = -1;
  for (const wanted of query) {
    const needle = fold(wanted);
    while (cursor < candidate.length && fold(candidate[cursor]) !== needle) cursor++;
    if (cursor === candidate.length) return null;
    score += p.baseScore;
    const prev = candidate[cursor - 1];
    if (cursor === 0 || prev === 0x2f || prev === 0x5f || prev === 0x2d || prev === 0x2e) {
      score += p.wordBoundaryBonus;
    }
    if (previous !== -1 && cursor === previous + 1) score += p.contiguityBonus;
    if (candidate[cursor] === wanted) score += p.exactCaseBonus;
    previous = cursor;
    cursor++;
  }
  score -= Math.min(candidate.length, Math.max(p.lengthCap, 0));
  return score;
}

// Byte-lexicographic compare of two UTF-8 byte arrays (matches C++ std::string <,
// which is byte order -- NOT JS string <, which is UTF-16 code-unit order).
function compareBytes(a, b) {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) {
    if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
  }
  return a.length - b.length;
}

// Rank candidates ({ id, label, detail }) for `query` using parameters `p`; returns
// the matching candidate indices in contract order (descending score; ties by label
// then id ascending, stable). Non-matches are dropped. Parameter fields may arrive as
// BigInt off the wire (the web decoder yields BigInt for signed integers), so they are
// normalized to Number here -- mixing BigInt with the numeric scores would throw.
export function fuzzyRank(candidates, query, params) {
  const p = {
    baseScore: Number(params.baseScore),
    wordBoundaryBonus: Number(params.wordBoundaryBonus),
    contiguityBonus: Number(params.contiguityBonus),
    exactCaseBonus: Number(params.exactCaseBonus),
    lengthCap: Number(params.lengthCap),
  };
  const q = enc.encode(query);
  const scored = [];
  for (let i = 0; i < candidates.length; i++) {
    const label = enc.encode(candidates[i].label);
    const id = enc.encode(candidates[i].id);
    const ls = fuzzyScore(label, q, p);
    const is = fuzzyScore(id, q, p);
    if (ls === null && is === null) continue;
    const score = Math.max(ls === null ? -Infinity : ls, is === null ? -Infinity : is);
    scored.push({ index: i, score, label, id });
  }
  scored.sort((l, r) => {
    if (l.score !== r.score) return r.score - l.score;
    const byLabel = compareBytes(l.label, r.label);
    if (byLabel !== 0) return byLabel;
    return compareBytes(l.id, r.id);
  });
  return scored.map((e) => e.index);
}
