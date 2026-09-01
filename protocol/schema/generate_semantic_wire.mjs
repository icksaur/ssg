import fs from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const defaults = Object.freeze({
  manifest: path.join(here, 'semantic_wire.mjs'),
  cpp: path.join(
    root, 'include/core/ssg/detail/generated/semantic_wire_manifest.h'),
  wireCpp: path.join(
    root, 'include/protocol/ssg/detail/generated/wire_schema.h'),
  replayCpp: path.join(
    root, 'include/core/ssg/detail/generated/ordinary_replay.h'),
  js: path.join(root, 'apps/web/generated/semantic_wire_manifest.mjs'),
});

const cppIdentifier = /^[A-Z][A-Za-z0-9]*$/;
const wireName = /^[a-z][a-z0-9]*(?:_[a-z0-9]+)*$/;
const wireFieldName = /^[a-z][A-Za-z0-9_]*$/;
const lifecycles = new Set(['current', 'compatibility', 'retired']);
const replayPolicies = new Set([
  'replacement', 'changed-replacement', 'specialized', 'compatibility',
]);
const specializedDecisions = new Set([
  'ordinary', 'payload', 'latency', 'asymptotic',
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
const wirePrimitiveKinds = new Set(['bool', 'bytes', 'int', 'uint', 'text']);
const wireCompositeKinds = new Set([
  'array', 'discriminated-record', 'enum', 'field-union', 'nullable', 'record',
  'ref', 'text-discriminated-record',
]);

function requireUnique(items, key, label) {
  const seen = new Set();
  for (const item of items) {
    const value = item[key];
    if (seen.has(value)) throw new Error(`duplicate ${label}: ${value}`);
    seen.add(value);
  }
}

function requireOnlyKeys(value, permitted, label) {
  const allowed = new Set(permitted);
  for (const key of Object.keys(value)) {
    if (!allowed.has(key)) throw new Error(`unknown ${label} property: ${key}`);
  }
}

const expressionKind = (expression) =>
  typeof expression === 'string' ? expression : expression?.kind;

function replayTypeIdentity(expression) {
  const kind = expressionKind(expression);
  if (wirePrimitiveKinds.has(kind)) {
    const declaration = typeof expression === 'string'
      ? { kind }
      : Object.fromEntries(
        Object.entries(expression).sort(([left], [right]) =>
          left.localeCompare(right)));
    return `primitive:${JSON.stringify(declaration)}`;
  }
  if (kind === 'ref') return `ref:${expression.type}`;
  return null;
}

export function resolveOrdinaryReplay(manifest) {
  const types = new Map(
    manifest.wireTypes.map((wireType) => [wireType.symbol, wireType.schema]));
  const snapshotRoot = types.get('SessionSnapshotSections');
  const deltaRoot = types.get('SessionDelta');
  if (expressionKind(snapshotRoot) !== 'record' ||
      expressionKind(deltaRoot) !== 'record') {
    throw new Error('ordinary replay requires session root records');
  }
  const findField = (schema, name) =>
    schema.fields.find((field) => field.wireName === name);
  const referencedRecord = (expression) => {
    if (expressionKind(expression) !== 'ref') return null;
    const schema = types.get(expression.type);
    return expressionKind(schema) === 'record' ? schema : null;
  };
  const nullable = (expression) =>
    expressionKind(expression) === 'nullable' ? expression.value : null;
  const fail = (section) => {
    throw new Error(`invalid ordinary replay shape: ${section.symbol}`);
  };
  const descriptors = [];
  for (const section of manifest.semanticSections) {
    if (section.replay !== 'changed-replacement' &&
        section.replay !== 'replacement') continue;
    if (section.delta.length !== 1) fail(section);
    const snapshotField = findField(snapshotRoot, section.snapshot);
    const deltaField = findField(deltaRoot, section.delta[0]);
    if (!snapshotField || !deltaField) fail(section);
    const nullableSnapshot = nullable(snapshotField.type);
    const stateType = nullableSnapshot ?? snapshotField.type;
    if (replayTypeIdentity(stateType) == null) fail(section);
    const common = {
      symbol: section.symbol,
      snapshot: section.snapshot,
      delta: section.delta[0],
      stateType,
      nullable: nullableSnapshot != null,
    };
    if (section.replay === 'changed-replacement') {
      if (snapshotField.required === common.nullable ||
          deltaField.required === common.nullable) fail(section);
      const change = referencedRecord(deltaField.type);
      if (!change || change.fields.length !== 2) fail(section);
      const changed = findField(change, 'changed');
      const replacement = findField(change, 'replacement');
      const replacementType = replacement && nullable(replacement.type);
      if (!changed || !changed.required ||
          expressionKind(changed.type) !== 'bool' ||
          !replacement || replacement.required || !replacementType ||
          replayTypeIdentity(replacementType) !== replayTypeIdentity(stateType)) {
        fail(section);
      }
      descriptors.push({ ...common, form: 'changed-replacement' });
      continue;
    }

    if (common.nullable) fail(section);
    const directType = nullable(deltaField.type);
    if (!deltaField.required && directType &&
        replayTypeIdentity(directType) === replayTypeIdentity(stateType)) {
      descriptors.push({ ...common, form: 'direct-replacement' });
      continue;
    }
    if (!snapshotField.required) fail(section);
    const envelope = deltaField.required
      ? referencedRecord(deltaField.type)
      : null;
    const fields = envelope?.fields ?? [];
    const replacement = fields.length === 1
      ? findField(envelope, 'replacement')
      : null;
    const replacementType = replacement && nullable(replacement.type);
    if (!replacement || replacement.required || !replacementType ||
        replayTypeIdentity(replacementType) !== replayTypeIdentity(stateType)) {
      fail(section);
    }
    descriptors.push({ ...common, form: 'replacement-envelope' });
  }
  return descriptors;
}

export function validateManifest(manifest) {
  const messages = manifest?.messageKinds;
  const sections = manifest?.semanticSections;
  const wireEnums = manifest?.wireEnums;
  const wireTypes = manifest?.wireTypes;
  if (!Array.isArray(messages) || !Array.isArray(sections) ||
      !Array.isArray(wireEnums) || !Array.isArray(wireTypes)) {
    throw new Error(
      'manifest requires messageKinds, semanticSections, wireEnums, and wireTypes arrays');
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
    if (section.replay === 'specialized') {
      if (!specializedDecisions.has(section.retention)) {
        throw new Error(
          `missing specialized decision: ${section.symbol}`);
      }
    } else if (section.retention != null) {
      throw new Error(
        `non-specialized decision: ${section.symbol}`);
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

  requireUnique(wireTypes, 'symbol', 'wire type symbol');
  const typeSymbols = new Set(wireTypes.map((wireType) => wireType.symbol));
  const enumSymbols = new Set(wireEnums.map((wireEnum) => wireEnum.symbol));
  const validateFields = (fields, owner) => {
    if (!Array.isArray(fields)) {
      throw new Error(`invalid wire type fields: ${owner}`);
    }
    requireUnique(fields, 'wireName', 'wire type field');
    for (const field of fields) {
      if (!wireFieldName.test(field.wireName) ||
          typeof field.required !== 'boolean') {
        throw new Error(`invalid wire type field: ${owner}`);
      }
      validateExpression(field.type, owner);
    }
  };
  const validateExpression = (expression, owner) => {
    const kind = typeof expression === 'string' ? expression : expression?.kind;
    if (!wirePrimitiveKinds.has(kind) && !wireCompositeKinds.has(kind)) {
      throw new Error(`invalid wire type expression: ${owner}`);
    }
    if (wirePrimitiveKinds.has(kind)) {
      if (kind === 'bytes' && typeof expression !== 'object') {
        throw new Error(`invalid wire bytes length: ${owner}`);
      }
      if (typeof expression === 'object') {
        const options = {
          bool: ['kind'],
          bytes: ['kind', 'length'],
          int: ['kind', 'hostInt'],
          uint: [
            'kind', 'maxHostInt', 'maxUint32', 'maxValue', 'allowedValues',
          ],
          text: ['kind', 'nonEmpty'],
        }[kind];
        requireOnlyKeys(expression, options, 'wire primitive');
        for (const key of options.slice(1)) {
          if (key === 'allowedValues' || key === 'length' ||
              key === 'maxValue') continue;
          if (expression[key] != null && typeof expression[key] !== 'boolean') {
            throw new Error(`invalid wire primitive option: ${owner}`);
          }
        }
        if (expression.allowedValues != null &&
            (!Array.isArray(expression.allowedValues) ||
             expression.allowedValues.length === 0 ||
             expression.allowedValues.some(
               (value) => !Number.isSafeInteger(value) || value < 0))) {
          throw new Error(`invalid wire primitive values: ${owner}`);
        }
        if (expression.maxValue != null &&
            (!Number.isSafeInteger(expression.maxValue) ||
             expression.maxValue < 0)) {
          throw new Error(`invalid wire primitive maximum: ${owner}`);
        }
        if (kind === 'bytes' &&
            (!Number.isSafeInteger(expression.length) ||
             expression.length <= 0)) {
          throw new Error(`invalid wire bytes length: ${owner}`);
        }
      }
      return;
    }
    if (kind === 'enum') {
      requireOnlyKeys(
        expression, ['kind', 'enum', 'values', 'acceptUnknown'], 'wire enum');
      if (!enumSymbols.has(expression.enum) ||
          (expression.values != null &&
           expression.values !== 'current' &&
           expression.values !== 'reservations') ||
          (expression.acceptUnknown != null &&
           typeof expression.acceptUnknown !== 'boolean') ||
          (expression.acceptUnknown === true &&
           !fallbackWireEnums.has(expression.enum))) {
        throw new Error(`invalid wire type enum: ${owner}`);
      }
    } else if (kind === 'ref') {
      requireOnlyKeys(expression, ['kind', 'type', 'recursive'], 'wire reference');
      if (!typeSymbols.has(expression.type) ||
          (expression.recursive != null &&
           typeof expression.recursive !== 'boolean')) {
        throw new Error(`invalid wire type reference: ${owner}`);
      }
    } else if (kind === 'nullable') {
      requireOnlyKeys(expression, ['kind', 'value'], 'wire nullable');
      validateExpression(expression.value, owner);
    } else if (kind === 'array') {
      requireOnlyKeys(
        expression, ['kind', 'items', 'nonEmpty', 'length'], 'wire array');
      if (expression.nonEmpty != null &&
          typeof expression.nonEmpty !== 'boolean') {
        throw new Error(`invalid wire array option: ${owner}`);
      }
      if (expression.length != null) {
        const length = expression.length;
        if ((!Number.isSafeInteger(length) || length < 0) &&
            (!enumSymbols.has(length?.enum) ||
             (length.values != null && length.values !== 'current'))) {
          throw new Error(`invalid wire array length: ${owner}`);
        }
      }
      validateExpression(expression.items, owner);
    } else if (kind === 'record') {
      requireOnlyKeys(
        expression,
        ['kind', 'fields', 'unknownFields', 'forbiddenFields'],
        'wire record');
      if (expression.unknownFields !== 'allow' &&
          expression.unknownFields !== 'reject') {
        throw new Error(`invalid unknown-field policy: ${owner}`);
      }
      validateFields(expression.fields, owner);
      if (!Array.isArray(expression.forbiddenFields) ||
          expression.forbiddenFields.some(
            (field) => typeof field !== 'string' || field.length === 0) ||
          new Set(expression.forbiddenFields).size !==
            expression.forbiddenFields.length ||
          expression.forbiddenFields.some((name) =>
            expression.fields.some((field) => field.wireName === name))) {
        throw new Error(`invalid forbidden wire field: ${owner}`);
      }
      if (expression.unknownFields === 'reject' &&
          expression.fields.some((field) => !field.required)) {
        throw new Error(`optional field in exact wire record: ${owner}`);
      }
    } else if (kind === 'field-union') {
      requireOnlyKeys(
        expression,
        ['kind', 'fields', 'variants', 'unknownFields'],
        'wire field union');
      if (expression.unknownFields !== 'allow' &&
          expression.unknownFields !== 'reject') {
        throw new Error(`invalid unknown-field policy: ${owner}`);
      }
      validateFields(expression.fields ?? [], owner);
      if (expression.unknownFields === 'reject' &&
          (expression.fields ?? []).some((field) => !field.required)) {
        throw new Error(`optional field in exact wire union: ${owner}`);
      }
      if (!Array.isArray(expression.variants) ||
          expression.variants.length < 2) {
        throw new Error(`invalid wire field union: ${owner}`);
      }
      requireUnique(expression.variants, 'wireName', 'wire union variant');
      for (const variant of expression.variants) {
        if (!wireFieldName.test(variant.wireName)) {
          throw new Error(`invalid wire union variant: ${owner}`);
        }
        validateExpression(variant.type, owner);
      }
    } else if (kind === 'discriminated-record' ||
               kind === 'text-discriminated-record') {
      requireOnlyKeys(
        expression,
        ['kind', 'discriminator', 'fields', 'variants', 'unknownFields'],
        'discriminated wire record');
      requireOnlyKeys(
        expression.discriminator ?? {},
        kind === 'discriminated-record'
          ? ['wireName', 'enum'] : ['wireName'],
        'wire discriminator');
      if (expression.unknownFields !== 'allow' &&
          expression.unknownFields !== 'reject') {
        throw new Error(`invalid unknown-field policy: ${owner}`);
      }
      validateFields(expression.fields ?? [], owner);
      if (expression.unknownFields === 'reject' &&
          (expression.fields ?? []).some((field) => !field.required)) {
        throw new Error(`optional field in exact discriminated wire record: ${owner}`);
      }
      if (!wireFieldName.test(expression.discriminator?.wireName) ||
          (kind === 'discriminated-record' &&
           !enumSymbols.has(expression.discriminator?.enum)) ||
          !Array.isArray(expression.variants) ||
          expression.variants.length === 0) {
        throw new Error(`invalid discriminated wire type: ${owner}`);
      }
      requireUnique(expression.variants, 'value', 'wire discriminator value');
      const declaration = kind === 'discriminated-record'
        ? manifest.wireEnums.find(
          (wireEnum) => wireEnum.symbol === expression.discriminator.enum)
        : null;
      const commonFields = new Set([
        expression.discriminator.wireName,
        ...(expression.fields ?? []).map((field) => field.wireName),
      ]);
      for (const variant of expression.variants) {
        if ((kind === 'discriminated-record' &&
             !declaration.values.some((value) =>
               value.symbol === variant.value &&
               value.lifecycle !== 'retired')) ||
            (kind === 'text-discriminated-record' &&
             !wireName.test(variant.value))) {
          throw new Error(`invalid wire discriminator value: ${owner}`);
        }
        validateFields(variant.fields ?? [], owner);
        if (expression.unknownFields === 'reject' &&
            (variant.fields ?? []).some((field) => !field.required)) {
          throw new Error(
            `optional variant field in exact discriminated wire record: ${owner}`);
        }
        if ((variant.fields ?? []).some(
          (field) => commonFields.has(field.wireName))) {
          throw new Error(`duplicate discriminated wire field: ${owner}`);
        }
      }
    }
  };
  for (const wireType of wireTypes) {
    if (!cppIdentifier.test(wireType.symbol)) {
      throw new Error(`invalid wire type declaration: ${wireType.symbol ?? '?'}`);
    }
    requireOnlyKeys(
      wireType, ['symbol', 'schema', 'jsBuilder'], 'wire type declaration');
    if (wireType.jsBuilder != null && typeof wireType.jsBuilder !== 'boolean') {
      throw new Error(`invalid wire type builder: ${wireType.symbol}`);
    }
    validateExpression(wireType.schema, wireType.symbol);
  }
  const edges = new Map(wireTypes.map((wireType) => [wireType.symbol, []]));
  const collectReferences = (expression, owner) => {
    const kind = typeof expression === 'string' ? expression : expression?.kind;
    if (kind === 'ref') {
      edges.get(owner).push({ target: expression.type, recursive: !!expression.recursive });
      return;
    }
    if (kind === 'nullable') collectReferences(expression.value, owner);
    else if (kind === 'array') collectReferences(expression.items, owner);
    else if (kind === 'record') {
      for (const field of expression.fields) collectReferences(field.type, owner);
    } else if (kind === 'field-union') {
      for (const field of expression.fields ?? []) collectReferences(field.type, owner);
      for (const variant of expression.variants) collectReferences(variant.type, owner);
    } else if (kind === 'discriminated-record' ||
               kind === 'text-discriminated-record') {
      for (const field of expression.fields ?? []) collectReferences(field.type, owner);
      for (const variant of expression.variants) {
        for (const field of variant.fields ?? []) collectReferences(field.type, owner);
      }
    }
  };
  for (const wireType of wireTypes) collectReferences(wireType.schema, wireType.symbol);
  for (const [owner, references] of edges) {
    for (const reference of references) {
      if (reference.target === owner) {
        if (!reference.recursive) {
          throw new Error(`undeclared recursive wire type: ${owner}`);
        }
      } else if (reference.recursive) {
        throw new Error(`invalid recursive wire edge: ${owner}`);
      }
    }
  }
  const visiting = new Set();
  const visited = new Set();
  const visit = (symbol) => {
    if (visiting.has(symbol)) throw new Error(`multi-type wire cycle: ${symbol}`);
    if (visited.has(symbol)) return;
    visiting.add(symbol);
    for (const edge of edges.get(symbol)) {
      if (edge.target !== symbol) visit(edge.target);
    }
    visiting.delete(symbol);
    visited.add(symbol);
  };
  for (const symbol of typeSymbols) visit(symbol);
  resolveOrdinaryReplay(manifest);
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
const specializedDecisionCpp = (value) => ({
  ordinary: 'Ordinary',
  payload: 'Payload',
  latency: 'Latency',
  asymptotic: 'Asymptotic',
})[value] ?? 'None';

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
  let deltaOffset = 0;
  const sectionFacts = manifest.semanticSections.map((section) => {
    const fact = `    SemanticSectionFact{${quote(section.symbol)}, ` +
    `${quote(section.snapshot)}, ManifestLifecycle::` +
    `${lifecycleCpp(section.lifecycle)}, ReplayPolicy::` +
    `${replayCpp(section.replay)}, SpecializedDecision::` +
    `${specializedDecisionCpp(section.retention)}, ${deltaOffset}, ` +
    `${section.delta.length}},`;
    deltaOffset += section.delta.length;
    return fact;
  });
  const specializedSections = manifest.semanticSections
    .filter((section) => section.lifecycle === 'current' &&
      section.replay === 'specialized')
    .map((section) => section.symbol);
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
    const compatibilityOrdinals = wireEnum.values
      .filter((value) => value.lifecycle === 'compatibility')
      .map((value) =>
        `inline constexpr std::uint64_t k${wireEnum.symbol}${value.symbol}` +
        `CompatibilityOrdinal = ${value.ordinal};`);
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
        .map((value) => value.wireName)).join('\n')}\n};` +
      (compatibilityOrdinals.length === 0
        ? '' : `\n${compatibilityOrdinals.join('\n')}`);
  });
  return `// Generated by protocol/schema/generate_semantic_wire.mjs.
// Source: protocol/schema/semantic_wire.mjs. Do not edit.
#pragma once

#include <array>
#include <cstddef>
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

enum class SpecializedDecision : std::uint8_t {
    None,
    Ordinary,
    Payload,
    Latency,
    Asymptotic,
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
    SpecializedDecision retention;
    std::size_t deltaOffset;
    std::size_t deltaCount;
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

inline constexpr std::array kSpecializedSemanticSections{
${strings(specializedSections).join('\n')}
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

const lowerCamel = (name) =>
  name.replace(/_([a-z0-9])/g, (_, character) => character.toUpperCase());

function renderCppOrdinaryReplay(manifest) {
  const descriptors = resolveOrdinaryReplay(manifest);
  const operations = descriptors.map((descriptor) => {
    const state = lowerCamel(descriptor.snapshot);
    const delta = lowerCamel(descriptor.delta);
    if (descriptor.form === 'changed-replacement') {
      if (descriptor.nullable) {
        return `    {
        const auto& change = delta.${delta}();
        if (!change.changed && change.replacement) return std::nullopt;
        if (change.changed) next.${state} = change.replacement;
    }`;
      }
      return `    {
        const auto& change = delta.${delta}();
        if (change.changed != change.replacement.has_value()) return std::nullopt;
        if (change.replacement) next.${state} = *change.replacement;
    }`;
    }
    return `    {
        const auto& replacement = ordinaryReplacement(delta.${delta}());
        if (replacement) next.${state} = *replacement;
    }`;
  });
  return `// Generated by protocol/schema/generate_semantic_wire.mjs.
// Source: protocol/schema/semantic_wire.mjs. Do not edit.
#pragma once

#include <optional>

namespace ssg::detail::generated {

template <typename Delta>
decltype(auto) ordinaryReplacement(const Delta& delta) {
    if constexpr (requires { delta.replacement; }) {
        return (delta.replacement);
    } else {
        return (delta);
    }
}

// CONTRACT: This is the sole ordinary-section replay inventory. It returns a
// candidate and never mutates the retained base.
template <typename Sections, typename Delta>
std::optional<Sections> replayOrdinarySessionSections(
    const Sections& base, const Delta& delta) {
    auto next = base;
${operations.join('\n')}
    return next;
}

}  // namespace ssg::detail::generated
`;
}

function enumOrdinals(manifest, expression) {
  const declaration = manifest.wireEnums.find(
    (wireEnum) => wireEnum.symbol === expression.enum);
  return declaration.values
    .filter((value) =>
      expression.values === 'reservations' || value.lifecycle === 'current')
    .map((value) => value.ordinal);
}

function flattenWireExpressions(manifest) {
  const functions = [];
  const roots = new Map();
  const add = (expression, owner) => {
    const index = functions.length;
    functions.push(null);
    const kind = typeof expression === 'string' ? expression : expression.kind;
    const node = { index, kind, expression, owner };
    const addFields = (fields) => (fields ?? []).map((field) => ({
      ...field, validator: add(field.type, owner),
    }));
    if (kind === 'nullable') node.value = add(expression.value, owner);
    else if (kind === 'array') {
      node.items = add(expression.items, owner);
      node.length = Number.isSafeInteger(expression.length)
        ? expression.length
        : expression.length
          ? manifest.wireEnums.find(
            (wireEnum) => wireEnum.symbol === expression.length.enum)
            .values.filter((value) =>
              expression.length.values !== 'current' ||
              value.lifecycle === 'current').length
          : null;
    }
    else if (kind === 'record') node.fields = addFields(expression.fields);
    else if (kind === 'field-union') {
      node.fields = addFields(expression.fields);
      node.variants = expression.variants.map((variant) => ({
        ...variant, validator: add(variant.type, owner),
      }));
    } else if (kind === 'discriminated-record' ||
               kind === 'text-discriminated-record') {
      node.fields = addFields(expression.fields);
      node.variants = expression.variants.map((variant) => ({
        ...variant, fields: addFields(variant.fields),
      }));
    } else if (kind === 'enum') {
      node.ordinals = enumOrdinals(manifest, expression);
    }
    functions[index] = node;
    return index;
  };
  for (const wireType of manifest.wireTypes) {
    roots.set(wireType.symbol, add(wireType.schema, wireType.symbol));
  }
  return { functions, roots };
}

const cppWireValidatorName = (index) => `validateWireNode${index}`;
const jsWireValidatorName = (index) => `validateWireNode${index}`;

function renderCppWireValidators(manifest) {
  const { functions, roots } = flattenWireExpressions(manifest);
  const refRoot = (expression) => roots.get(expression.type);
  const call = (index, value) => `${cppWireValidatorName(index)}(${value})`;
  const renderField = (field) => {
    const pointer = `field${field.validator}`;
    if (field.required === false) {
      return `    if (const ProtocolValue* ${pointer} = value.field(${
        quote(field.wireName)}); ${pointer} && !${
        call(field.validator, `*${pointer}`)}) return false;`;
    }
    return `    const ProtocolValue* ${pointer} = value.field(${
      quote(field.wireName)});\n    if (!${pointer} || !${
      call(field.validator, `*${pointer}`)}) return false;`;
  };
  const bodies = functions.map((node) => {
    const { expression, kind } = node;
    if (kind === 'bool') return '    return value.asBool().has_value();';
    if (kind === 'bytes') {
      return `    const auto* bytes = value.asBytes();\n` +
        `    return bytes && bytes->size() == ${expression.length};`;
    }
    if (kind === 'int') {
      const base = 'value.asInt().has_value()';
      return expression.hostInt === true
        ? `    if (const auto raw = value.asInt()) {\n` +
          `        return *raw >= std::numeric_limits<int>::min() &&\n` +
          `               *raw <= std::numeric_limits<int>::max();\n` +
          `    }\n    return false;`
        : `    return ${base};`;
    }
    if (kind === 'uint') {
      const conditions = [];
      if (expression.maxHostInt === true) {
        conditions.push(
          '*raw <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())');
      }
      if (expression.maxUint32 === true) {
        conditions.push('*raw <= std::numeric_limits<std::uint32_t>::max()');
      }
      if (expression.maxValue != null) {
        conditions.push(`*raw <= ${expression.maxValue}`);
      }
      if (expression.allowedValues) {
        conditions.push(`(${expression.allowedValues.map(
          (value) => `*raw == ${value}`).join(' || ')})`);
      }
      return conditions.length > 0
        ? `    const auto raw = value.asUint();\n` +
          `    return raw && ${conditions.join(' && ')};`
        : '    return value.asUint().has_value();';
    }
    if (kind === 'text') {
      return expression.nonEmpty === true
        ? '    const auto* text = value.asText();\n' +
          '    return text && !text->empty();'
        : '    return value.asText() != nullptr;';
    }
    if (kind === 'enum') {
      const accepts = node.ordinals.map(
        (ordinal) => `*raw == ${ordinal}`).join(' || ');
      return `    const auto raw = value.asUint();\n` +
        `    return raw && (${expression.acceptUnknown === true ? 'true' : accepts});`;
    }
    if (kind === 'ref') {
      return `    return ${call(refRoot(expression), 'value')};`;
    }
    if (kind === 'nullable') {
      return `    return value.kind() == ProtocolValue::Kind::NullValue ||\n` +
        `           ${call(node.value, 'value')};`;
    }
    if (kind === 'array') {
      return `    const auto* array = value.asArray();\n` +
        `    if (!array${expression.nonEmpty === true ? ' || array->empty()' : ''}${
          node.length != null ? ` || array->size() != ${node.length}` : ''}) return false;\n` +
        `    return std::ranges::all_of(*array, [](const ProtocolValue& item) {\n` +
        `        return ${call(node.items, 'item')};\n    });`;
    }
    if (kind === 'record') {
      const forbidden = expression.forbiddenFields.map(
        (name) => `value.field(${quote(name)})`).join(' || ');
      return `    const auto* object = value.asObject();\n` +
        `    if (!object${
          expression.unknownFields === 'reject'
            ? ` || object->size() != ${node.fields.length}` : ''}${
          forbidden ? ` || ${forbidden}` : ''}) return false;\n${
        node.fields.map(renderField).join('\n')}\n    return true;`;
    }
    if (kind === 'field-union') {
      const variantChecks = node.variants.map((variant) => {
        const variable = `variant${variant.validator}`;
        return `    if (const ProtocolValue* ${variable} = value.field(${
          quote(variant.wireName)}); ${variable} &&\n` +
          `        ${variable}->kind() != ProtocolValue::Kind::NullValue) {\n` +
          `        ++variantCount;\n` +
          `        if (!${call(variant.validator, `*${variable}`)}) return false;\n` +
          `    }`;
      }).join('\n');
      return `    const auto* object = value.asObject();\n` +
        `    if (!object${
          expression.unknownFields === 'reject'
            ? ` || object->size() != ${node.fields.length + 1}` : ''}) return false;\n${
        node.fields.map(renderField).join('\n')}\n` +
        `    std::size_t variantCount = 0;\n${variantChecks}\n` +
        `    return variantCount == 1;`;
    }
    if (kind === 'discriminated-record' ||
        kind === 'text-discriminated-record') {
      const declaration = kind === 'discriminated-record'
        ? manifest.wireEnums.find(
          (wireEnum) => wireEnum.symbol === expression.discriminator.enum)
        : null;
      if (kind === 'discriminated-record') {
        return `    const auto* object = value.asObject();\n` +
          `    if (!object) return false;\n${
          node.fields.map(renderField).join('\n')}\n` +
          `    const ProtocolValue* discriminator = value.field(${
            quote(expression.discriminator.wireName)});\n` +
          `    if (!discriminator || !discriminator->asUint()) return false;\n` +
          (expression.unknownFields === 'reject'
            ? `    const auto fieldCount = object->size();\n` : '') +
          `    switch (*discriminator->asUint()) {\n${node.variants.map(
            (variant) => {
              const ordinal = declaration.values.find(
                (value) => value.symbol === variant.value).ordinal;
              const checks = variant.fields.map(renderField).join('\n');
              const count = 1 + node.fields.length + variant.fields.length;
              return `    case ${ordinal}: {\n${
                expression.unknownFields === 'reject'
                  ? `        if (fieldCount != ${count}) return false;\n` : ''}${
                checks ? `\n${checks}` : ''}\n        return true;\n    }`;
            }).join('\n')}\n` +
          `    default: return false;\n    }`;
      }
      return `    const auto* object = value.asObject();\n` +
        `    if (!object) return false;\n${
        node.fields.map(renderField).join('\n')}\n` +
        `    const ProtocolValue* discriminatorField = value.field(${
          quote(expression.discriminator.wireName)});\n` +
        `    if (!discriminatorField || !discriminatorField->asText()) return false;\n` +
        `    const std::string& discriminator = *discriminatorField->asText();\n` +
        (expression.unknownFields === 'reject'
          ? `    const auto fieldCount = object->size();\n` : '') +
        `${node.variants.map((variant) => {
          const checks = variant.fields.map(renderField).join('\n');
          const count = 1 + node.fields.length + variant.fields.length;
          return `    if (discriminator == ${quote(variant.value)}) {\n${
            expression.unknownFields === 'reject'
              ? `        if (fieldCount != ${count}) return false;\n` : ''}${
            checks ? `\n${checks}` : ''}\n        return true;\n    }`;
        }).join('\n')}\n    return false;`;
    }
    throw new Error(`unsupported C++ wire validator kind: ${kind}`);
  });
  const declarations = functions.map((node) =>
    `inline bool ${cppWireValidatorName(node.index)}(const ProtocolValue& value);`);
  const definitions = functions.map((node) =>
    `inline bool ${cppWireValidatorName(node.index)}(const ProtocolValue& value) {\n${
      bodies[node.index]}\n}`);
  const exports = manifest.wireTypes.map((wireType) =>
    `inline bool validate${wireType.symbol}Wire(const ProtocolValue& value) {\n` +
    `    return ${call(roots.get(wireType.symbol), 'value')};\n}`);
  return `// Generated by protocol/schema/generate_semantic_wire.mjs.
// Source: protocol/schema/semantic_wire.mjs. Do not edit.
#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <ranges>

#include <ssg/Protocol.h>

namespace ssg::detail::generated {

${declarations.join('\n')}

${definitions.join('\n\n')}

${exports.join('\n\n')}

}  // namespace ssg::detail::generated
`;
}

function renderJsWireValidators(manifest) {
  const { functions, roots } = flattenWireExpressions(manifest);
  const call = (index, value) => `${jsWireValidatorName(index)}(${value})`;
  const renderField = (field) => {
    const access = `value[${quote(field.wireName)}]`;
    return field.required === false
      ? `    if (${access} !== undefined && !${
        call(field.validator, access)}) return false;`
      : `    if (${access} === undefined || !${
        call(field.validator, access)}) return false;`;
  };
  const bodies = functions.map((node) => {
    const { expression, kind } = node;
    if (kind === 'bool') return '    return typeof value === \'boolean\';';
    if (kind === 'bytes') {
      return `    return value instanceof Uint8Array && value.length === ${
        expression.length};`;
    }
    if (kind === 'int') {
      return expression.hostInt === true
        ? '    if (typeof value !== \'number\' && typeof value !== \'bigint\') return false;\n' +
          '    const raw = Number(value);\n' +
          '    return Number.isSafeInteger(raw) && raw >= -2147483648 && raw <= 2147483647;'
        : `    if (typeof value === 'bigint') {\n` +
          `      return value >= -9223372036854775808n &&\n` +
          `        value <= 9223372036854775807n;\n` +
          `    }\n` +
          `    return typeof value === 'number' && Number.isSafeInteger(value);`;
    }
    if (kind === 'uint') {
      const numberConditions = ['Number.isSafeInteger(value)', 'value >= 0'];
      const bigintConditions = [
        'value >= 0n', 'value <= 18446744073709551615n',
      ];
      if (expression.maxHostInt === true) {
        numberConditions.push('value <= 2147483647');
        bigintConditions.push('value <= 2147483647n');
      }
      if (expression.maxUint32 === true) {
        numberConditions.push('value <= 4294967295');
        bigintConditions.push('value <= 4294967295n');
      }
      if (expression.maxValue != null) {
        numberConditions.push(`value <= ${expression.maxValue}`);
        bigintConditions.push(`value <= ${expression.maxValue}n`);
      }
      if (expression.allowedValues) {
        numberConditions.push(`(${expression.allowedValues.map(
          (allowed) => `value === ${allowed}`).join(' || ')})`);
        bigintConditions.push(`(${expression.allowedValues.map(
          (allowed) => `value === ${allowed}n`).join(' || ')})`);
      }
      return `    if (typeof value === 'bigint') {\n` +
        `      return ${bigintConditions.join(' && ')};\n` +
        `    }\n` +
        `    return typeof value === 'number' &&\n` +
        `      ${numberConditions.join(' && ')};`;
    }
    if (kind === 'text') {
      return `    return typeof value === 'string'${
        expression.nonEmpty === true ? ' && value.length > 0' : ''};`;
    }
    if (kind === 'enum') {
      const accepts = node.ordinals.map(
        (ordinal) => `raw === ${ordinal}`).join(' || ');
      return `    if (typeof value !== 'number' && typeof value !== 'bigint') return false;\n` +
        `    const raw = Number(value);\n` +
        `    return Number.isSafeInteger(raw) && raw >= 0 && (${
          expression.acceptUnknown === true ? 'true' : accepts});`;
    }
    if (kind === 'ref') return `    return ${call(roots.get(expression.type), 'value')};`;
    if (kind === 'nullable') {
      return `    return value === null || ${call(node.value, 'value')};`;
    }
    if (kind === 'array') {
      return `    return Array.isArray(value)${
        expression.nonEmpty === true ? ' && value.length > 0' : ''} &&\n` +
        `${node.length != null ? `      value.length === ${node.length} &&\n` : ''}` +
        `      value.every((item) => ${call(node.items, 'item')});`;
    }
    if (kind === 'record') {
      const forbidden = expression.forbiddenFields.map(
        (name) => `Object.hasOwn(value, ${quote(name)})`).join(' || ');
      return `    if (!value || typeof value !== 'object' || Array.isArray(value)${
        expression.unknownFields === 'reject'
          ? ` || Object.keys(value).length !== ${node.fields.length}` : ''}${
        forbidden ? ` || ${forbidden}` : ''}) return false;\n${
        node.fields.map(renderField).join('\n')}\n    return true;`;
    }
    if (kind === 'field-union') {
      const checks = node.variants.map((variant) => {
        const access = `value[${quote(variant.wireName)}]`;
        return `    if (${access} !== undefined && ${access} !== null) {\n` +
          `      ++variantCount;\n` +
          `      if (!${call(variant.validator, access)}) return false;\n    }`;
      }).join('\n');
      return `    if (!value || typeof value !== 'object' || Array.isArray(value)${
        expression.unknownFields === 'reject'
          ? ` || Object.keys(value).length !== ${node.fields.length + 1}` : ''}) return false;\n${
        node.fields.map(renderField).join('\n')}\n` +
        `    let variantCount = 0;\n${checks}\n` +
        `    return variantCount === 1;`;
    }
    if (kind === 'discriminated-record' ||
        kind === 'text-discriminated-record') {
      const declaration = kind === 'discriminated-record'
        ? manifest.wireEnums.find(
          (wireEnum) => wireEnum.symbol === expression.discriminator.enum)
        : null;
      const cases = node.variants.map((variant) => {
        const label = kind === 'discriminated-record'
          ? declaration.values.find(
            (value) => value.symbol === variant.value).ordinal
          : quote(variant.value);
        return `    case ${label}:\n${
          variant.fields.map(renderField).join('\n')}\n      return true;`;
      }).join('\n');
      return `    if (!value || typeof value !== 'object' || Array.isArray(value)) return false;\n${
        node.fields.map(renderField).join('\n')}\n` +
        `    const discriminatorValue = value[${
          quote(expression.discriminator.wireName)}];\n` +
        (kind === 'discriminated-record'
          ? `    if (typeof discriminatorValue !== 'number' &&\n` +
            `        typeof discriminatorValue !== 'bigint') return false;\n` +
            `    const discriminator = Number(discriminatorValue);\n` +
            `    if (!Number.isSafeInteger(discriminator) || discriminator < 0) return false;\n`
          : `    if (typeof discriminatorValue !== 'string') return false;\n` +
            `    const discriminator = discriminatorValue;\n`) +
        `    switch (discriminator) {\n${
          node.variants.map((variant) => {
            const label = kind === 'discriminated-record'
              ? declaration.values.find(
                (value) => value.symbol === variant.value).ordinal
              : quote(variant.value);
            const checks = variant.fields.map(renderField).join('\n');
            const count = 1 + node.fields.length + variant.fields.length;
            return `    case ${label}:\n${
              expression.unknownFields === 'reject'
                ? `      if (Object.keys(value).length !== ${count}) return false;\n`
                : ''}${checks}\n      return true;`;
          }).join('\n')}\n    default: return false;\n    }`;
    }
    throw new Error(`unsupported JavaScript wire validator kind: ${kind}`);
  });
  const camelName = (name) => name.replace(
    /_([a-z0-9])/g, (_, character) => character.toUpperCase());
  const convert = (index, source, seen = new Set()) => {
    const node = functions[index];
    const { expression, kind } = node;
    if (kind === 'bool') return source;
    if (kind === 'int' || kind === 'uint' || kind === 'enum') {
      return `BigInt(${source})`;
    }
    if (kind === 'text') return `String(${source})`;
    if (kind === 'nullable') {
      return `(${source} == null ? null : ${
        convert(node.value, source, seen)})`;
    }
    if (kind === 'array') {
      return `${source}.map((item) => ${convert(node.items, 'item', seen)})`;
    }
    if (kind === 'record') {
      return `({ ${node.fields.map((field) =>
        `${field.wireName}: ${convert(
          field.validator, `${source}.${camelName(field.wireName)}`, seen)}`)
        .join(', ')} })`;
    }
    if (kind === 'ref') {
      const target = roots.get(expression.type);
      if (seen.has(target)) {
        throw new Error(`cannot generate recursive JavaScript builder: ${expression.type}`);
      }
      return convert(target, source, new Set([...seen, target]));
    }
    throw new Error(`cannot generate JavaScript builder for ${kind}`);
  };
  const builders = manifest.wireTypes.flatMap((wireType) => {
    if (!wireType.jsBuilder) return [];
    const root = functions[roots.get(wireType.symbol)];
    if (root.kind === 'record') {
      const params = root.fields.map(
        (field) => camelName(field.wireName));
      return [
        `export const build${wireType.symbol}Wire = (${
          params.join(', ')}) => ({\n${root.fields.map((field) =>
          `  ${field.wireName}: ${convert(
            field.validator, camelName(field.wireName))},`).join('\n')}\n});`,
      ];
    }
    if (root.kind === 'discriminated-record' ||
        root.kind === 'text-discriminated-record') {
      const declaration = root.kind === 'discriminated-record'
        ? manifest.wireEnums.find(
          (wireEnum) => wireEnum.symbol === root.expression.discriminator.enum)
        : null;
      return root.variants.map((variant) => {
        const fields = [...root.fields, ...variant.fields];
        const params = fields.map((field) => camelName(field.wireName));
        const discriminator = root.kind === 'discriminated-record'
          ? `${declaration.values.find(
            (value) => value.symbol === variant.value).ordinal}n`
          : quote(variant.value);
        return `export const build${wireType.symbol}${variant.value}Wire = (${
          params.join(', ')}) => ({\n` +
          `  ${root.expression.discriminator.wireName}: ${discriminator},\n${
            fields.map((field) => `  ${field.wireName}: ${
              convert(field.validator, camelName(field.wireName))},`).join('\n')}\n});`;
      });
    }
    throw new Error(`unsupported JavaScript builder type: ${wireType.symbol}`);
  });
  return `\n${functions.map((node) =>
    `function ${jsWireValidatorName(node.index)}(value) {\n${
      bodies[node.index]}\n}`).join('\n\n')}\n\n${
    manifest.wireTypes.map((wireType) =>
      `export const validate${wireType.symbol}Wire = (value) => ${
        call(roots.get(wireType.symbol), 'value')};`).join('\n')}\n\n${
    builders.join('\n\n')}\n`;
}

function renderJsOrdinaryReplay(manifest) {
  const descriptors = resolveOrdinaryReplay(manifest);
  const valueCheck = (descriptor, name) => {
    const kind = expressionKind(descriptor.stateType);
    if (kind === 'ref') {
      return `validate${descriptor.stateType.type}Wire(${name})`;
    }
    return {
      bool: `typeof ${name} === 'boolean'`,
      bytes: `${name} instanceof Uint8Array && ${name}.length === ${
        descriptor.stateType.length}`,
      int: `Number.isInteger(${name}) || typeof ${name} === 'bigint'`,
      uint: `(Number.isInteger(${name}) && ${name} >= 0) || ` +
        `(typeof ${name} === 'bigint' && ${name} >= 0n)`,
      text: `typeof ${name} === 'string'`,
    }[kind] ?? 'false';
  };
  const operations = descriptors.map((descriptor) => {
    const field = quote(descriptor.delta);
    const state = descriptor.snapshot;
    if (descriptor.form === 'changed-replacement') {
      const presence = descriptor.nullable
        ? 'if (present) {'
        : 'if (!present) return null;';
      const closePresence = descriptor.nullable ? '\n    }' : '';
      const replacement = valueCheck(descriptor, 'replacement');
      return `  {
    const present = Object.prototype.hasOwnProperty.call(delta, ${field});
    ${presence}
    const change = delta[${field}];
    if (!change || typeof change !== 'object' || Array.isArray(change) ||
        typeof change.changed !== 'boolean') return null;
    const replacement = change.replacement;
    if (!change.changed) {
      if (replacement != null) return null;
    } else {${
      descriptor.nullable
        ? `\n      if (replacement != null && !(${replacement})) return null;\n` +
          `      next.${state} = replacement ?? null;`
        : `\n      if (replacement == null || !(${replacement})) return null;\n` +
          `      next.${state} = replacement;`}
    }
${closePresence}
  }`;
    }
    if (descriptor.form === 'replacement-envelope') {
      const replacement = valueCheck(descriptor, 'replacement');
      return `  {
    if (!Object.prototype.hasOwnProperty.call(delta, ${field})) return null;
    const envelope = delta[${field}];
    if (!envelope || typeof envelope !== 'object' || Array.isArray(envelope)) {
      return null;
    }
    const replacement = envelope.replacement;
    if (replacement != null) {
      if (!(${replacement})) return null;
      next.${state} = replacement;
    }
  }`;
    }
    const replacement = valueCheck(descriptor, 'replacement');
    return `  {
    if (Object.prototype.hasOwnProperty.call(delta, ${field})) {
      const replacement = delta[${field}];
      if (replacement != null) {
        if (!(${replacement})) return null;
        next.${state} = replacement;
      }
    }
  }`;
  });
  return `
// CONTRACT: This is the sole ordinary-section replay inventory. It returns a
// candidate and never mutates the retained base.
export function replayOrdinarySessionSections(baseSections, delta) {
  if (!baseSections || typeof baseSections !== 'object' ||
      !delta || typeof delta !== 'object') return null;
  const next = { ...baseSections };
${operations.join('\n')}
  return next;
}
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
${renderJsWireValidators(manifest)}
${renderJsOrdinaryReplay(manifest)}
`;
}

export function renderOutputs(manifest) {
  validateManifest(manifest);
  return {
    cpp: renderCpp(manifest),
    wireCpp: renderCppWireValidators(manifest),
    replayCpp: renderCppOrdinaryReplay(manifest),
    js: renderJs(manifest),
  };
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
    else if (argument === '--wire-cpp') options.wireCpp = args[++index];
    else if (argument === '--replay-cpp') options.replayCpp = args[++index];
    else if (argument === '--js') options.js = args[++index];
    else throw new Error(`unknown argument: ${argument}`);
  }
  return options;
}

export async function run(args) {
  const options = parseArguments(args);
  const outputs = renderOutputs(await loadManifest(options.manifest));
  const destinations = {
    cpp: options.cpp, wireCpp: options.wireCpp,
    replayCpp: options.replayCpp, js: options.js,
  };
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
