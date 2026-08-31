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
  uiCpp: path.join(
    root, 'include/ssg/detail/generated/ui_wire_schema.h'),
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
const wirePrimitiveKinds = new Set(['bool', 'int', 'uint', 'text']);
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
      if (!wireName.test(field.wireName) ||
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
      if (typeof expression === 'object') {
        const options = {
          bool: ['kind'],
          int: ['kind', 'hostInt'],
          uint: ['kind', 'maxHostInt'],
          text: ['kind', 'nonEmpty'],
        }[kind];
        requireOnlyKeys(expression, options, 'wire primitive');
        for (const key of options.slice(1)) {
          if (expression[key] != null && typeof expression[key] !== 'boolean') {
            throw new Error(`invalid wire primitive option: ${owner}`);
          }
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
      requireOnlyKeys(expression, ['kind', 'items', 'nonEmpty'], 'wire array');
      if (expression.nonEmpty != null &&
          typeof expression.nonEmpty !== 'boolean') {
        throw new Error(`invalid wire array option: ${owner}`);
      }
      validateExpression(expression.items, owner);
    } else if (kind === 'record') {
      requireOnlyKeys(
        expression, ['kind', 'fields', 'unknownFields'], 'wire record');
      if (expression.unknownFields !== 'allow') {
        throw new Error(`invalid unknown-field policy: ${owner}`);
      }
      validateFields(expression.fields, owner);
    } else if (kind === 'field-union') {
      requireOnlyKeys(
        expression,
        ['kind', 'fields', 'variants', 'unknownFields'],
        'wire field union');
      if (expression.unknownFields !== 'allow') {
        throw new Error(`invalid unknown-field policy: ${owner}`);
      }
      validateFields(expression.fields ?? [], owner);
      if (!Array.isArray(expression.variants) ||
          expression.variants.length < 2) {
        throw new Error(`invalid wire field union: ${owner}`);
      }
      requireUnique(expression.variants, 'wireName', 'wire union variant');
      for (const variant of expression.variants) {
        if (!wireName.test(variant.wireName)) {
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
      if (expression.unknownFields !== 'allow') {
        throw new Error(`invalid unknown-field policy: ${owner}`);
      }
      validateFields(expression.fields ?? [], owner);
      if (!wireName.test(expression.discriminator?.wireName) ||
          (kind === 'discriminated-record' &&
           !enumSymbols.has(expression.discriminator?.enum)) ||
          !Array.isArray(expression.variants) ||
          expression.variants.length === 0) {
        throw new Error(`invalid discriminated wire type: ${owner}`);
      }
      requireUnique(expression.variants, 'value', 'wire discriminator value');
      const declaration = kind === 'discriminated-record'
        ? wireEnums.find(
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
               value.lifecycle === 'current')) ||
            (kind === 'text-discriminated-record' &&
             !wireName.test(variant.value))) {
          throw new Error(`invalid wire discriminator value: ${owner}`);
        }
        validateFields(variant.fields ?? [], owner);
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
    else if (kind === 'array') node.items = add(expression.items, owner);
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
      return expression.maxHostInt === true
        ? '    const auto raw = value.asUint();\n' +
          '    return raw && *raw <= static_cast<std::uint64_t>(\n' +
          '        std::numeric_limits<int>::max());'
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
        `    if (!array${expression.nonEmpty === true ? ' || array->empty()' : ''}) return false;\n` +
        `    return std::ranges::all_of(*array, [](const ProtocolValue& item) {\n` +
        `        return ${call(node.items, 'item')};\n    });`;
    }
    if (kind === 'record') {
      return `    if (!value.asObject()) return false;\n${
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
      return `    if (!value.asObject()) return false;\n${
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
      const cases = node.variants.map((variant) => {
        if (kind === 'discriminated-record') {
          const ordinal = declaration.values.find(
            (value) => value.symbol === variant.value).ordinal;
          return `    case ${ordinal}: {\n${
            variant.fields.map(renderField).join('\n')}\n        return true;\n    }`;
        }
        return `    if (discriminator == ${quote(variant.value)}) {\n${
          variant.fields.map(renderField).join('\n')}\n        return true;\n    }`;
      }).join('\n');
      if (kind === 'discriminated-record') {
        return `    if (!value.asObject()) return false;\n${
          node.fields.map(renderField).join('\n')}\n` +
          `    const ProtocolValue* discriminator = value.field(${
            quote(expression.discriminator.wireName)});\n` +
          `    if (!discriminator || !discriminator->asUint()) return false;\n` +
          `    switch (*discriminator->asUint()) {\n${cases}\n` +
          `    default: return false;\n    }`;
      }
      return `    if (!value.asObject()) return false;\n${
        node.fields.map(renderField).join('\n')}\n` +
        `    const ProtocolValue* discriminatorField = value.field(${
          quote(expression.discriminator.wireName)});\n` +
        `    if (!discriminatorField || !discriminatorField->asText()) return false;\n` +
        `    const std::string& discriminator = *discriminatorField->asText();\n` +
        `${cases}\n    return false;`;
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
    if (kind === 'int') {
      return expression.hostInt === true
        ? '    if (typeof value !== \'number\' && typeof value !== \'bigint\') return false;\n' +
          '    const raw = Number(value);\n' +
          '    return Number.isSafeInteger(raw) && raw >= -2147483648 && raw <= 2147483647;'
        : '    return (typeof value === \'number\' || typeof value === \'bigint\') &&\n' +
          '      Number.isSafeInteger(Number(value));';
    }
    if (kind === 'uint') {
      if (expression.maxHostInt === true) {
        return '    if (typeof value !== \'number\' && typeof value !== \'bigint\') return false;\n' +
          '    const raw = Number(value);\n' +
          '    return Number.isSafeInteger(raw) && raw >= 0 && raw <= 2147483647;';
      }
      return `    if (typeof value === 'bigint') {\n` +
        `      return value >= 0n && value <= 18446744073709551615n;\n` +
        `    }\n` +
        `    return typeof value === 'number' && Number.isSafeInteger(value) &&\n` +
        `      value >= 0;`;
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
        `      value.every((item) => ${call(node.items, 'item')});`;
    }
    if (kind === 'record') {
      return `    if (!value || typeof value !== 'object' || Array.isArray(value)) return false;\n${
        node.fields.map(renderField).join('\n')}\n    return true;`;
    }
    if (kind === 'field-union') {
      const checks = node.variants.map((variant) => {
        const access = `value[${quote(variant.wireName)}]`;
        return `    if (${access} !== undefined && ${access} !== null) {\n` +
          `      ++variantCount;\n` +
          `      if (!${call(variant.validator, access)}) return false;\n    }`;
      }).join('\n');
      return `    if (!value || typeof value !== 'object' || Array.isArray(value)) return false;\n${
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
          cases}\n    default: return false;\n    }`;
    }
    throw new Error(`unsupported JavaScript wire validator kind: ${kind}`);
  });
  return `\n${functions.map((node) =>
    `function ${jsWireValidatorName(node.index)}(value) {\n${
      bodies[node.index]}\n}`).join('\n\n')}\n\n${
    manifest.wireTypes.map((wireType) =>
      `export const validate${wireType.symbol}Wire = (value) => ${
        call(roots.get(wireType.symbol), 'value')};`).join('\n')}\n`;
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
`;
}

export function renderOutputs(manifest) {
  validateManifest(manifest);
  return {
    cpp: renderCpp(manifest),
    uiCpp: renderCppWireValidators(manifest),
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
    else if (argument === '--ui-cpp') options.uiCpp = args[++index];
    else if (argument === '--js') options.js = args[++index];
    else throw new Error(`unknown argument: ${argument}`);
  }
  return options;
}

export async function run(args) {
  const options = parseArguments(args);
  const outputs = renderOutputs(await loadManifest(options.manifest));
  const destinations = { cpp: options.cpp, uiCpp: options.uiCpp, js: options.js };
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
