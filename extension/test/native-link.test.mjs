// Unit tests for the protocol check: node --test extension/test/native-link.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { PROTOCOL, protocolMismatch } from '../src/background/native-link.js';

test('a module whose range contains PROTOCOL is compatible', () => {
  assert.equal(protocolMismatch({ protocolMin: PROTOCOL, protocolMax: PROTOCOL }), null);
  assert.equal(protocolMismatch({ protocolMin: PROTOCOL - 1, protocolMax: PROTOCOL + 2 }), null);
});

test('a module that requires a newer protocol asks for an extension update', () => {
  assert.equal(protocolMismatch({ protocolMin: PROTOCOL + 1, protocolMax: PROTOCOL + 1 }), 'extension');
});

test('a module that only speaks older protocols asks for a native update', () => {
  assert.equal(protocolMismatch({ protocolMin: PROTOCOL - 1, protocolMax: PROTOCOL - 1 }), 'native');
});

test('modules that only report `protocol` are judged by it', () => {
  assert.equal(protocolMismatch({ protocol: PROTOCOL }), null);
  assert.equal(protocolMismatch({ protocol: PROTOCOL + 1 }), 'extension');
});
