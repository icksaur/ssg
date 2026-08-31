import fs from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const defaults = Object.freeze({
  manifest: path.join(here, 'semantic_wire.mjs'),
  cpp: path.join(
    root, 'include/ssg/detail/generated/semantic_wire_manifest.h'),
  js: path.join(root, 'apps/web/generated/semantic_wire_manifest.mjs'),
});

const cppIdentifier = /^[A-Z][A-Za-z0-9]*$/;
const wireName = /^[a-z][a-z0-9]*(?:_[a-z0-9]+)*$/;
const lifecycles = new Set(['current', 'compatibility', 'retired']);
const replayPolicies = new Set([
  'replacement', 'changed-replacement', 'specialized', 'compatibility',
]);
const enumUnknownPolicies = new Set(['reject', 'map-to']);
const fallbackWireEnums = new Set(['ScrollAxis']);
const fixedJsExports = new Set([
  'MESSAGE_KINDS',
  'PROTOCOL_MESSAGE_KIND',
  'SEMANTIC_SECTIONS',
  'SEMANTIC_SNAPSHOT_FIELDS',
  'SEMANTIC_DELTA_FIELDS',
]);

function requireUnique(items, key, label) {
  const seen = new Set();
  for (const item of items) {
    const value = item[key];
    if (seen.has(value)) throw new Error(`duplicate ${label}: ${value}`);
    seen.add(value);
  }
}

export function validateManifest(manifest) {
  const messages = manifest?.messageKinds;
  const sections = manifest?.semanticSections;
  const wireEnums = manifest?.wireEnums;
  if (!Array.isArray(messages) || !Array.isArray(sections) ||
      !Array.isArray(wireEnums)) {
    throw new Error(
      'manifest requires messageKinds, semanticSections, and wireEnums arrays');
  }
  requireUnique(messages, 'symbol', 'message symbol');
  requireUnique(messages, 'wireName', 'message wire name');
  requireUnique(messages, 'ordinal', 'message ordinal');
  for (const message of messages) {
    if (!cppIdentifier.test(message.symbol) ||
        !wireName.test(message.wireName) ||
        !Number.isSafeInteger(message.ordinal) || message.ordinal < 0 ||
        !lifecycles.has(message.lifecycle)) {
      throw new Error(`invalid message declaration: ${message.symbol ?? '?'}`);
    }
  }

  requireUnique(sections, 'symbol', 'section symbol');
  requireUnique(sections, 'snapshot', 'snapshot field');
  const deltaFields = new Set();
  for (const section of sections) {
    if (!cppIdentifier.test(section.symbol) ||
        !wireName.test(section.snapshot) ||
        !Array.isArray(section.delta) || section.delta.length === 0 ||
        section.delta.some((field) => !wireName.test(field)) ||
        !lifecycles.has(section.lifecycle) ||
        !replayPolicies.has(section.replay) ||
        section.lifecycle === 'retired' ||
        ((section.lifecycle === 'compatibility') !==
         (section.replay === 'compatibility'))) {
      throw new Error(`invalid section declaration: ${section.symbol ?? '?'}`);
    }
    for (const field of section.delta) {
      if (deltaFields.has(field)) throw new Error(`duplicate delta field: ${field}`);
      deltaFields.add(field);
    }
  }

  requireUnique(wireEnums, 'symbol', 'wire enum symbol');
  requireUnique(wireEnums, 'jsName', 'wire enum JavaScript name');
  for (const wireEnum of wireEnums) {
    if (!cppIdentifier.test(wireEnum.symbol) ||
        !/^[A-Z][A-Z0-9_]*$/.test(wireEnum.jsName) ||
        !Array.isArray(wireEnum.values) || wireEnum.values.length === 0 ||
        !wireEnum.unknown ||
        !enumUnknownPolicies.has(wireEnum.unknown.policy)) {
      throw new Error(`invalid wire enum declaration: ${wireEnum.symbol ?? '?'}`);
    }
    if (fixedJsExports.has(wireEnum.jsName)) {
      throw new Error(`reserved wire enum JavaScript name: ${wireEnum.jsName}`);
    }
    requireUnique(wireEnum.values, 'symbol', 'wire enum value symbol');
    requireUnique(wireEnum.values, 'wireName', 'wire enum value wire name');
    requireUnique(wireEnum.values, 'ordinal', 'wire enum ordinal');
    for (const value of wireEnum.values) {
      if (!cppIdentifier.test(value.symbol) ||
          !wireName.test(value.wireName) ||
          !Number.isSafeInteger(value.ordinal) || value.ordinal < 0 ||
          !lifecycles.has(value.lifecycle)) {
        throw new Error(
          `invalid wire enum value: ${wireEnum.symbol}.${value.symbol ?? '?'}`);
      }
    }
    const fallback = wireEnum.unknown.fallback;
    if ((wireEnum.unknown.policy === 'reject' && fallback != null) ||
        (wireEnum.unknown.policy === 'map-to' &&
         (!fallbackWireEnums.has(wireEnum.symbol) ||
          !wireEnum.values.some((value) =>
            value.symbol === fallback && value.lifecycle === 'current')))) {
      throw new Error(`invalid wire enum fallback: ${wireEnum.symbol}`);
    }
  }
  return manifest;
}

const quote = (value) => JSON.stringify(value);
const lifecycleCpp = (value) => ({
  current: 'Current',
  compatibility: 'Compatibility',
  retired: 'Retired',
})[value];
const replayCpp = (value) => ({
  replacement: 'Replacement',
  'changed-replacement': 'ChangedReplacement',
  specialized: 'Specialized',
  compatibility: 'Compatibility',
})[value];

function renderCpp(manifest) {
  const currentMessages = manifest.messageKinds.filter(
    (message) => message.lifecycle !== 'retired');
  const snapshots = manifest.semanticSections.map((section) => section.snapshot);
  const deltas = manifest.semanticSections.flatMap((section) => section.delta);
  const enumLines = currentMessages.map(
    (message, index) =>
      `    ${index === 0 ? '' : 'X'}(${message.symbol}, ${message.ordinal})`);
  if (enumLines.length > 0) enumLines[0] =
    `    X(${currentMessages[0].symbol}, ${currentMessages[0].ordinal})`;
  const messageFacts = manifest.messageKinds.map((message) =>
    `    ProtocolMessageKindFact{${quote(message.symbol)}, ` +
    `${quote(message.wireName)}, ${message.ordinal}, ` +
    `ManifestLifecycle::${lifecycleCpp(message.lifecycle)}},`);
  const sectionFacts = manifest.semanticSections.map((section) =>
    `    SemanticSectionFact{${quote(section.symbol)}, ` +
    `${quote(section.snapshot)}, ManifestLifecycle::` +
    `${lifecycleCpp(section.lifecycle)}, ReplayPolicy::` +
    `${replayCpp(section.replay)}},`);
  const strings = (values) => values.map(
    (value) => `    std::string_view{${quote(value)}},`);
  const enumMacroName = (wireEnum) =>
    `SSG_${wireEnum.jsName}_ENUMERATORS`;
  const enumMacros = manifest.wireEnums.map((wireEnum) => {
    const current = wireEnum.values.filter(
      (value) => value.lifecycle === 'current');
    const lines = current.map(
      (value) => `    X(${value.symbol}, ${value.ordinal})`);
    return `#define ${enumMacroName(wireEnum)}(X) \\\n${
      lines.map((line, index) =>
        `${line}${index + 1 === lines.length ? '' : ' \\'}`).join('\n')}`;
  });
  const enumFacts = manifest.wireEnums.map((wireEnum) => {
    const facts = (values) => values.map((value) =>
      `    WireEnumValueFact{${quote(value.symbol)}, ${quote(value.wireName)}, ` +
      `${value.ordinal}, ManifestLifecycle::${lifecycleCpp(value.lifecycle)}},`);
    const currentValues = wireEnum.values.filter(
      (value) => value.lifecycle === 'current');
    const fallback = wireEnum.unknown.policy === 'map-to'
      ? wireEnum.values.find(
        (value) => value.symbol === wireEnum.unknown.fallback).ordinal
      : 0;
    return `inline constexpr WireEnumFact k${wireEnum.symbol}WireEnum{\n` +
      `    ${quote(wireEnum.symbol)}, UnknownEnumPolicy::${
        wireEnum.unknown.policy === 'map-to' ? 'MapTo' : 'Reject'}, ` +
      `${fallback},\n};\n` +
      `inline constexpr std::array k${wireEnum.symbol}WireValues{\n` +
      `${facts(currentValues).join('\n')}\n};\n` +
      `inline constexpr std::array k${wireEnum.symbol}WireReservations{\n` +
      `${facts(wireEnum.values).join('\n')}\n};\n` +
      `inline constexpr std::array k${wireEnum.symbol}CurrentWireNames{\n` +
      `${strings(currentValues
        .map((value) => value.wireName)).join('\n')}\n};`;
  });
  return `// Generated by protocol/schema/generate_semantic_wire.mjs.
// Source: protocol/schema/semantic_wire.mjs. Do not edit.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#define SSG_PROTOCOL_MESSAGE_KIND_ENUMERATORS(X) \\
${enumLines.map((line, index) =>
    `${line}${index + 1 === enumLines.length ? '' : ' \\'}`).join('\n')}

${enumMacros.join('\n\n')}

namespace ssg::detail::generated {

enum class ManifestLifecycle : std::uint8_t {
    Current,
    Compatibility,
    Retired,
};

enum class ReplayPolicy : std::uint8_t {
    Replacement,
    ChangedReplacement,
    Specialized,
    Compatibility,
};

enum class UnknownEnumPolicy : std::uint8_t {
    Reject,
    MapTo,
};

struct ProtocolMessageKindFact {
    std::string_view symbol;
    std::string_view wireName;
    std::uint8_t ordinal;
    ManifestLifecycle lifecycle;
};

struct SemanticSectionFact {
    std::string_view symbol;
    std::string_view snapshotField;
    ManifestLifecycle lifecycle;
    ReplayPolicy replay;
};

struct WireEnumFact {
    std::string_view symbol;
    UnknownEnumPolicy unknownPolicy;
    std::uint64_t fallbackOrdinal;
};

struct WireEnumValueFact {
    std::string_view symbol;
    std::string_view wireName;
    std::uint64_t ordinal;
    ManifestLifecycle lifecycle;
};

inline constexpr std::array kProtocolMessageKinds{
${messageFacts.join('\n')}
};

inline constexpr std::array kSemanticSections{
${sectionFacts.join('\n')}
};

inline constexpr std::array kSemanticSnapshotFields{
${strings(snapshots).join('\n')}
};

inline constexpr std::array kSemanticDeltaFields{
${strings(deltas).join('\n')}
};

${enumFacts.join('\n\n')}

}  // namespace ssg::detail::generated
`;
}

function renderJs(manifest) {
  return `// Generated by protocol/schema/generate_semantic_wire.mjs.
// Source: protocol/schema/semantic_wire.mjs. Do not edit.
const deepFreeze = (value) => {
  for (const child of Object.values(value)) {
    if (child && typeof child === 'object') deepFreeze(child);
  }
  return Object.freeze(value);
};

export const MESSAGE_KINDS = deepFreeze(${
  JSON.stringify(manifest.messageKinds, null, 2)});
export const PROTOCOL_MESSAGE_KIND = deepFreeze({
${manifest.messageKinds
    .filter((message) => message.lifecycle === 'current')
    .map((message) =>
      `  ${message.symbol.replace(/([a-z0-9])([A-Z])/g, '$1_$2').toUpperCase()}: ` +
      `${message.ordinal},`)
    .join('\n')}
});
export const SEMANTIC_SECTIONS = deepFreeze(${
  JSON.stringify(manifest.semanticSections, null, 2)});
export const SEMANTIC_SNAPSHOT_FIELDS = Object.freeze(
  SEMANTIC_SECTIONS.map((section) => section.snapshot));
export const SEMANTIC_DELTA_FIELDS = Object.freeze(
  SEMANTIC_SECTIONS.flatMap((section) => section.delta));
${manifest.wireEnums.map((wireEnum) => {
    const values = wireEnum.values
      .filter((value) => value.lifecycle === 'current')
      .map((value) =>
        `  ${value.symbol.replace(/([a-z0-9])([A-Z])/g, '$1_$2').toUpperCase()}: ` +
        `${value.ordinal},`);
    return `export const ${wireEnum.jsName} = deepFreeze({\n${
      values.join('\n')}\n});`;
  }).join('\n')}
`;
}

export function renderOutputs(manifest) {
  validateManifest(manifest);
  return { cpp: renderCpp(manifest), js: renderJs(manifest) };
}

async function readIfPresent(file) {
  try {
    return await fs.readFile(file, 'utf8');
  } catch (error) {
    if (error.code === 'ENOENT') return null;
    throw error;
  }
}

export async function checkOutputs(outputs, destinations) {
  const stale = [];
  for (const name of Object.keys(outputs)) {
    if (await readIfPresent(destinations[name]) !== outputs[name]) {
      stale.push(destinations[name]);
    }
  }
  if (stale.length > 0) {
    throw new Error(`generated semantic wire output is stale:\n${stale.join('\n')}`);
  }
}

export async function writeOutputs(outputs, destinations) {
  const suffix = `.tmp-${process.pid}-${Date.now()}`;
  const prepared = [];
  try {
    for (const name of Object.keys(outputs)) {
      const destination = destinations[name];
      await fs.mkdir(path.dirname(destination), { recursive: true });
      const temporary = destination + suffix;
      const backup = destination + suffix + '.backup';
      await fs.writeFile(temporary, outputs[name], 'utf8');
      const previous = await readIfPresent(destination);
      if (previous != null) await fs.writeFile(backup, previous, 'utf8');
      prepared.push({ destination, temporary, backup, previous });
    }
    const attempted = [];
    try {
      for (const item of prepared) {
        attempted.push(item);
        await fs.copyFile(item.temporary, item.destination);
      }
    } catch (error) {
      for (const item of attempted.reverse()) {
        if (item.previous == null) await fs.rm(item.destination, { force: true });
        else await fs.copyFile(item.backup, item.destination);
      }
      throw error;
    }
  } finally {
    await Promise.all(prepared.flatMap((item) => [
      fs.rm(item.temporary, { force: true }),
      fs.rm(item.backup, { force: true }),
    ]));
  }
}

export async function loadManifest(file) {
  const module = await import(
    pathToFileURL(path.resolve(file)).href + `?generated=${Date.now()}`);
  return module.default ?? module;
}

function parseArguments(args) {
  const options = { mode: 'check', ...defaults };
  for (let index = 0; index < args.length; ++index) {
    const argument = args[index];
    if (argument === '--check') options.mode = 'check';
    else if (argument === '--write') options.mode = 'write';
    else if (argument === '--manifest') options.manifest = args[++index];
    else if (argument === '--cpp') options.cpp = args[++index];
    else if (argument === '--js') options.js = args[++index];
    else throw new Error(`unknown argument: ${argument}`);
  }
  return options;
}

export async function run(args) {
  const options = parseArguments(args);
  const outputs = renderOutputs(await loadManifest(options.manifest));
  const destinations = { cpp: options.cpp, js: options.js };
  if (options.mode === 'write') await writeOutputs(outputs, destinations);
  else await checkOutputs(outputs, destinations);
}

if (process.argv[1] &&
    path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  run(process.argv.slice(2)).catch((error) => {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  });
}
