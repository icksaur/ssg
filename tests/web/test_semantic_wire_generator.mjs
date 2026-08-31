import assert from 'node:assert/strict';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import manifest from '../../protocol/schema/semantic_wire.mjs';
import {
  checkOutputs,
  renderOutputs,
  run,
  validateManifest,
  writeOutputs,
} from '../../protocol/schema/generate_semantic_wire.mjs';

let checks = 0;
const check = async (name, test) => {
  await test();
  checks++;
};

const temporary = await fsp.mkdtemp(
  path.join(os.tmpdir(), 'ssg-semantic-wire-'));
try {
  await check('rendering is deterministic and check detects drift', async () => {
    const first = renderOutputs(manifest);
    const second = renderOutputs(manifest);
    assert.deepEqual(first, second);
    const destinations = {
      cpp: path.join(temporary, 'generated', 'manifest.h'),
      js: path.join(temporary, 'generated', 'manifest.mjs'),
    };
    await writeOutputs(first, destinations);
    await checkOutputs(second, destinations);
    const generated = await import(pathToFileURL(destinations.js).href);
    assert.equal(Object.isFrozen(generated.MESSAGE_KINDS[0]), true);
    assert.equal(Object.isFrozen(generated.SEMANTIC_SECTIONS[0]), true);
    assert.equal(Object.isFrozen(generated.SEMANTIC_SECTIONS[0].delta), true);
    await fsp.appendFile(destinations.js, '// stale\n');
    await assert.rejects(
      checkOutputs(second, destinations), /output is stale/);
  });

  await check('invalid declarations reject before rendering', async () => {
    const duplicate = structuredClone(manifest);
    duplicate.messageKinds[1].ordinal = duplicate.messageKinds[0].ordinal;
    assert.throws(() => validateManifest(duplicate), /duplicate message ordinal/);

    const retiredSection = structuredClone(manifest);
    retiredSection.semanticSections[0].lifecycle = 'retired';
    assert.throws(
      () => validateManifest(retiredSection), /invalid section declaration/);

    const mismatchedCompatibility = structuredClone(manifest);
    mismatchedCompatibility.semanticSections[0].lifecycle = 'compatibility';
    assert.throws(
      () => validateManifest(mismatchedCompatibility),
      /invalid section declaration/);

    const invalidManifest = path.join(temporary, 'invalid-manifest.mjs');
    const invalidDestinations = path.join(temporary, 'invalid-output');
    await fsp.writeFile(invalidManifest, `
export default {
  messageKinds: [
    { symbol: 'Same', wireName: 'first', ordinal: 0, lifecycle: 'current' },
    { symbol: 'Same', wireName: 'second', ordinal: 1, lifecycle: 'current' },
  ],
  semanticSections: [],
};
`);
    await assert.rejects(run([
      '--write',
      '--manifest', invalidManifest,
      '--cpp', path.join(invalidDestinations, 'manifest.h'),
      '--js', path.join(invalidDestinations, 'manifest.mjs'),
    ]), /duplicate message symbol/);
    await assert.rejects(fsp.access(invalidDestinations), { code: 'ENOENT' });
  });

  await check('retired message reservations remain explicit and sparse', async () => {
    assert.deepEqual(
      manifest.messageKinds
        .filter((message) => message.lifecycle === 'retired')
        .map(({ symbol, wireName, ordinal }) => [symbol, wireName, ordinal]),
      [
        ['ClipboardRequest', 'clipboard_request', 3],
        ['ClipboardResponse', 'clipboard_response', 4],
        ['StatusActionInvocation', 'status_action_invocation', 5],
      ]);
  });

  await check('lifecycle metadata stays outside runtime consumers', async () => {
    const root = path.resolve(
      path.dirname(fileURLToPath(import.meta.url)), '../..');
    const protocol = fs.readFileSync(path.join(root, 'src/Protocol.cpp'), 'utf8');
    const reconcile =
      fs.readFileSync(path.join(root, 'apps/web/reconcile.mjs'), 'utf8');
    assert.doesNotMatch(protocol, /ManifestLifecycle|\\.lifecycle/);
    assert.doesNotMatch(reconcile, /ManifestLifecycle|\\.lifecycle/);
  });
} finally {
  await fsp.rm(temporary, { recursive: true, force: true });
}

process.stdout.write(`semantic wire generator: ${checks} checks passed\n`);
