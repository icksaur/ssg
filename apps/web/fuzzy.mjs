// The shared fuzzy match-and-order contract, executed locally in the browser client
// for responsiveness. This mirrors the C++ reference (ssg::referenceRank in
// PaletteSearcher): a client narrows the library-published candidate universe by the
// user's live query with NO round trip, and MUST reproduce the library's match set
// and ordering byte-for-byte. Iteration and length are over raw UTF-8 BYTES (not
// UTF-16 code units); folding is ASCII-only. The scoring weights and length cap are
// NOT hardcoded here -- they arrive on the wire as the palette section's parameters
// and are passed in.

const enc = new TextEncoder();

function inDomain(value, lo, hi) {
  return Number.isInteger(value) && value >= lo && value <= hi;
}

// True iff every weight is within +/-maxMagnitude and lengthCap is within
// [0, maxMagnitude]. `maxMagnitude` is the library-owned bound published on the
// palette wire section (max_parameter_magnitude), NOT a constant hardcoded here, so
// the client's accepted domain cannot drift from the library's.
export function matcherParametersInDomain(p, maxMagnitude) {
  const m = maxMagnitude;
  return (
    Number.isInteger(m) && m > 0 &&
    inDomain(p.baseScore, -m, m) &&
    inDomain(p.wordBoundaryBonus, -m, m) &&
    inDomain(p.contiguityBonus, -m, m) &&
    inDomain(p.exactCaseBonus, -m, m) &&
    inDomain(p.lengthCap, 0, m)
  );
}

// ASCII-only fold: A-Z -> a-z, every other byte (including UTF-8 continuation bytes)
// folds to itself. Deliberately not locale-aware, so this and the C++ reference agree.
function fold(byte) {
  return byte >= 0x41 && byte <= 0x5a ? byte + 0x20 : byte;
}

// Score `query` as a case-folded subsequence of `candidate` (both UTF-8 byte arrays);
// null when not a subsequence. Weights come from `p`; `scoredCap` caps the candidate
// bytes scored so the score stays bounded (mirrors the C++ cap, which is the same
// published magnitude), keeping the double score exact and equal to C++.
function fuzzyScore(candidate, query, p, scoredCap) {
  if (query.length === 0) return 0;
  const scoredSize = Math.min(candidate.length, scoredCap);
  let score = 0;
  let cursor = 0;
  let previous = -1;
  for (const wanted of query) {
    const needle = fold(wanted);
    while (cursor < scoredSize && fold(candidate[cursor]) !== needle) cursor++;
    if (cursor === scoredSize) return null;
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
  score -= Math.min(scoredSize, Math.max(p.lengthCap, 0));
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
// then id ascending, stable). Non-matches are dropped. `maxMagnitude` is the palette
// section's published max_parameter_magnitude (the library-owned domain bound and the
// scored-byte cap in one). Parameter fields may arrive as BigInt off the wire, so they
// are normalized to Number first -- mixing BigInt with numeric scores would throw.
export function fuzzyRank(candidates, query, params, maxMagnitude) {
  const cap = Number(maxMagnitude);
  const p = {
    baseScore: Number(params.baseScore),
    wordBoundaryBonus: Number(params.wordBoundaryBonus),
    contiguityBonus: Number(params.contiguityBonus),
    exactCaseBonus: Number(params.exactCaseBonus),
    lengthCap: Number(params.lengthCap),
  };
  // Enforce the published domain; a frame the server would never publish (or a
  // corrupted one) is refused loudly rather than scored divergently.
  if (!matcherParametersInDomain(p, cap)) {
    throw new RangeError('matcher parameters out of domain');
  }
  const q = enc.encode(query);
  const scored = [];
  for (let i = 0; i < candidates.length; i++) {
    const label = enc.encode(candidates[i].label);
    const id = enc.encode(candidates[i].id);
    const ls = fuzzyScore(label, q, p, cap);
    const is = fuzzyScore(id, q, p, cap);
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
