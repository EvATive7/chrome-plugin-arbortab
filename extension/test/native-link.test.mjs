// Unit tests for the protocol check and the bridge failure classification: node --test extension/test/native-link.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { PROTOCOL, bridgeFailure, protocolMismatch } from '../src/background/native-link.js';

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

test('an unregistered bridge is reported as missing', () => {
  assert.equal(bridgeFailure('Specified native messaging host not found.'), 'missing');
});

test('the reason the bridge sent wins over the disconnect error', () => {
  assert.equal(bridgeFailure('Native host has exited.', 'module-not-loaded'), 'module-not-loaded');
});

test('other disconnects are plain failures', () => {
  assert.equal(bridgeFailure('Native host has exited.'), 'failed');
  assert.equal(bridgeFailure(undefined), 'failed');
});
