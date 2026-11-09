// Unit tests for the protocol check and the bridge failure classification: node --test extension/test/native-link.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import {
  NativeLink,
  PROTOCOL,
  bridgeFailure,
  nativeOutdated,
  nativeUpdatePromptDecision,
  protocolMismatch,
} from '../src/background/native-link.js';

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

test('every hello is a handshake, but only a compatible hello starts normal work', () => {
  const calls = [];
  const link = new NativeLink({
    onHandshake: () => calls.push('handshake'),
    onHello: () => calls.push('hello'),
  });
  link.receive({
    type: 'hello',
    protocolMin: PROTOCOL + 1,
    protocolMax: PROTOCOL + 1,
  });
  assert.deepEqual(calls, ['handshake']);

  link.receive({ type: 'hello', protocolMin: PROTOCOL, protocolMax: PROTOCOL });
  assert.deepEqual(calls, ['handshake', 'handshake', 'hello']);
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

test('a module older than the extension is outdated', () => {
  assert.equal(nativeOutdated('0.2.0', '0.2.1'), true);
  assert.equal(nativeOutdated('0.9.9', '1.0.0'), true);
  assert.equal(nativeOutdated('0.2.9', '0.10.0'), true);
});

test('a module as new as or newer than the extension is not outdated', () => {
  assert.equal(nativeOutdated('0.2.1', '0.2.1'), false);
  assert.equal(nativeOutdated('0.3.0', '0.2.1'), false);
  assert.equal(nativeOutdated('0.10.0', '0.9.0'), false);
});

test('an unreadable version is never reported as outdated', () => {
  assert.equal(nativeOutdated(undefined, '0.2.1'), false);
  assert.equal(nativeOutdated('?', '0.2.1'), false);
  assert.equal(nativeOutdated('0.2', '0.2.1'), false);
});

test('the native update prompt is suppressed on initial installation', () => {
  assert.deepEqual(nativeUpdatePromptDecision({
    nativeVersion: '0.2.0',
    extensionVersion: '0.2.1',
    initialInstall: true,
  }), { key: '0.2.1|0.2.0', shouldOpen: false });
});

test('the native update prompt is not repeated for a shown version pair', () => {
  assert.equal(nativeUpdatePromptDecision({
    nativeVersion: '0.2.0',
    extensionVersion: '0.2.1',
    shownKeys: ['0.2.1|0.2.0'],
  }).shouldOpen, false);
});

test('the native update prompt may open after either version changes', () => {
  assert.equal(nativeUpdatePromptDecision({
    nativeVersion: '0.2.0',
    extensionVersion: '0.2.2',
    shownKeys: ['0.2.1|0.2.0'],
  }).shouldOpen, true);
  assert.equal(nativeUpdatePromptDecision({
    nativeVersion: '0.2.1',
    extensionVersion: '0.2.2',
    shownKeys: ['0.2.2|0.2.0'],
  }).shouldOpen, true);
});

test('the native update prompt stays closed when the module is not older', () => {
  assert.equal(nativeUpdatePromptDecision({
    nativeVersion: '0.2.1',
    extensionVersion: '0.2.1',
  }).shouldOpen, false);
  assert.equal(nativeUpdatePromptDecision({
    nativeVersion: '0.3.0',
    extensionVersion: '0.2.1',
  }).shouldOpen, false);
});
