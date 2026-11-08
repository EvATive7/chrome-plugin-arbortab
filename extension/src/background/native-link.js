// Connection to the ArborTab native module.
//
// Preferred transport: Native Messaging. Chrome starts arbortab-bridge.exe for us, and the
// bridge connects to the named pipe of the browser process that started it, so every browser
// instance talks to its own module. Fallback: the module's WebSocket on 127.0.0.1, for
// installs without the bridge. Both carry the same messages (AGENTS.md, "通信协议").

const NATIVE_HOST = 'com.arbortab.bridge';
const PORTS = [47821, 47822, 47823, 47824, 47825];
// Message protocol spoken by this extension. Must stay within the native module's
// [protocolMin, protocolMax] (native/src/link/protocol.h); see AGENTS.md, "版本与兼容".
export const PROTOCOL = 1;
const NO_FEATURES = { layout: false, twisty: false, paint: false, drag: false };

/**
 * Which side has to be updated, or null when the module speaks PROTOCOL. Modules predating
 * protocolMin/protocolMax only report `protocol`.
 */
export function protocolMismatch(message) {
  const min = message.protocolMin ?? message.protocol ?? PROTOCOL;
  const max = message.protocolMax ?? message.protocol ?? PROTOCOL;
  if (PROTOCOL < min) {
    return 'extension';
  }
  if (PROTOCOL > max) {
    return 'native';
  }
  return null;
}

/**
 * Why a Native Messaging attempt ended before the module answered: 'missing' (the bridge is
 * not registered), 'module-not-loaded' (bridge ran, but this browser has no module), or
 * 'failed' (anything else).
 */
export function bridgeFailure(lastErrorMessage, bridgeReason) {
  if (bridgeReason) {
    return bridgeReason;
  }
  return /not found/i.test(lastErrorMessage || '') ? 'missing' : 'failed';
}

export class NativeLink {
  constructor({ onMessage, onStatus, onHello, onEvent } = {}) {
    this.onMessage = onMessage || (() => {});
    this.onStatus = onStatus || (() => {});
    this.onHello = onHello || (() => {});
    /** Connection milestones for diagnostics: (event, data). */
    this.onEvent = onEvent || (() => {});
    /** Active connection: { transport: 'pipe'|'websocket', send(message), close() }. */
    this.channel = null;
    /** Bound WebSocket port when connected over WebSocket. */
    this.port = null;
    /** Outcome of the last Native Messaging attempt: 'ok', 'missing', 'module-not-loaded', 'failed'. */
    this.bridge = null;
    this.features = { ...NO_FEATURES };
    this.connected = false;
    /** Last hello/status from the module: nativeVersion, offsets {status, schema, chromeVersion}. */
    this.native = null;
    /** 'extension' or 'native' when the protocols do not overlap; nothing is sent then. */
    this.updateNeeded = null;
    this.portIndex = 0;
    this.retryDelay = 500;
    this.heartbeat = null;
    this.queue = [];
    /** Pending request() calls, each waiting for one reply type. */
    this.waiters = [];
    this.stopped = false;
  }

  get isConnected() {
    return this.connected;
  }

  get transport() {
    return this.channel?.transport ?? null;
  }

  start() {
    this.stopped = false;
    this.connect();
  }

  stop() {
    this.stopped = true;
    const channel = this.channel;
    this.down();
    channel?.close();
  }

  connect() {
    if (this.stopped) {
      return;
    }
    if (chrome.runtime.connectNative) {
      this.connectBridge();
    } else {
      this.connectWebSocket();
    }
  }

  connectBridge() {
    let port;
    try {
      port = chrome.runtime.connectNative(NATIVE_HOST);
    } catch (error) {
      this.bridge = bridgeFailure(String(error));
      this.connectWebSocket();
      return;
    }
    let opened = false;
    let reason = null;
    const channel = {
      transport: 'pipe',
      send: (message) => port.postMessage(message),
      close: () => port.disconnect(),
    };
    port.onMessage.addListener((message) => {
      if (message?.type === 'bridge.unavailable') {
        reason = message.reason || 'failed';
        return;  // the bridge exits right after; onDisconnect falls back
      }
      if (!opened) {
        opened = true;
        this.bridge = 'ok';
        this.opened(channel);
      }
      this.receive(message);
    });
    port.onDisconnect.addListener(() => {
      const error = chrome.runtime.lastError?.message;
      if (opened) {
        if (this.channel === channel) {
          this.down();
          this.scheduleReconnect();
        }
        return;
      }
      // Never reached a module this way: an install without the bridge may still have the
      // WebSocket endpoint.
      this.bridge = bridgeFailure(error, reason);
      this.connectWebSocket();
    });
    // The bridge forwards this as soon as it reaches the pipe; the module answers with hello.
    port.postMessage(this.helloMessage());
  }

  connectWebSocket() {
    if (this.stopped) {
      return;
    }
    const port = PORTS[this.portIndex % PORTS.length];
    let socket;
    try {
      socket = new WebSocket(`ws://127.0.0.1:${port}/arbortab`);
    } catch {
      this.scheduleReconnect();
      return;
    }
    const channel = {
      transport: 'websocket',
      send: (message) => socket.send(JSON.stringify(message)),
      close: () => socket.close(),
    };
    let failed = false;
    socket.addEventListener('open', () => {
      this.port = port;
      this.portIndex = 0;
      this.opened(channel);
      this.send(this.helloMessage());
    });
    socket.addEventListener('message', (event) => {
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        return;
      }
      this.receive(message);
    });
    const onDown = () => {
      if (failed) {
        return;
      }
      failed = true;
      if (this.channel === channel) {
        this.down();
      } else if (this.channel) {
        return;  // another transport is live
      }
      // Try the next port on reconnect: the browser may have moved to another one.
      this.portIndex += 1;
      this.scheduleReconnect();
    };
    socket.addEventListener('close', onDown);
    socket.addEventListener('error', onDown);
  }

  helloMessage() {
    return { type: 'hello', protocol: PROTOCOL, extVersion: chrome.runtime.getManifest().version };
  }

  opened(channel) {
    this.channel = channel;
    this.connected = true;
    this.retryDelay = 500;
    this.onEvent('connected', { transport: channel.transport, port: this.port });
    for (const message of this.queue.splice(0)) {
      this.send(message);
    }
    // Keeps the service worker alive and detects a dead WebSocket.
    this.heartbeat = setInterval(() => this.send({ type: 'ping' }), 20000);
    this.onStatus({ connected: true, features: this.features });
  }

  down() {
    if (!this.channel) {
      return;
    }
    this.onEvent('disconnected', { transport: this.channel.transport });
    this.channel = null;
    this.connected = false;
    this.port = null;
    if (this.heartbeat) {
      clearInterval(this.heartbeat);
      this.heartbeat = null;
    }
    this.onStatus({ connected: false, features: this.features });
  }

  receive(message) {
    if (message.type === 'hello' || message.type === 'status') {
      this.updateNeeded = protocolMismatch(message);
      this.native = {
        nativeVersion: message.nativeVersion,
        offsets: message.offsets || null,
        protocolMin: message.protocolMin ?? message.protocol,
        protocolMax: message.protocolMax ?? message.protocol,
        safeMode: message.safeMode || null,
      };
      this.features = this.updateNeeded ? { ...NO_FEATURES } : { ...NO_FEATURES, ...message.features };
      this.onEvent(message.type, {
        ...this.native,
        features: this.features,
        ...(this.updateNeeded ? { updateNeeded: this.updateNeeded } : {}),
      });
      if (message.type === 'hello' && !this.updateNeeded) {
        this.onHello(message);
      }
      this.onStatus({ connected: true, features: this.features, nativeVersion: message.nativeVersion });
      return;
    }
    const waiter = this.waiters.findIndex((item) => item.type === message.type);
    if (waiter >= 0) {
      this.waiters.splice(waiter, 1)[0].resolve(message);
      return;
    }
    if (!this.updateNeeded) {
      this.onMessage(message);
    }
  }

  scheduleReconnect() {
    if (this.stopped || this.reconnectTimer) {
      return;
    }
    const delay = this.retryDelay;
    this.retryDelay = Math.min(this.retryDelay * 2, 30000);
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect();
    }, delay);
  }

  /** Sends `message` and resolves with the first reply of `replyType`, or null on timeout. */
  request(message, replyType, timeoutMs = 3000) {
    if (!this.connected) {
      return Promise.resolve(null);
    }
    return new Promise((resolve) => {
      const waiter = {
        type: replyType,
        resolve: (reply) => {
          clearTimeout(timer);
          resolve(reply);
        },
      };
      const timer = setTimeout(() => {
        this.waiters = this.waiters.filter((item) => item !== waiter);
        resolve(null);
      }, timeoutMs);
      this.waiters.push(waiter);
      this.send(message);
    });
  }

  send(message) {
    // debug.report is read-only, so it is still allowed when the protocols do not match.
    const always = ['hello', 'ping', 'debug.report'];
    if (this.updateNeeded && !always.includes(message.type)) {
      return false;  // an incompatible module must not receive messages it would misread
    }
    if (!this.channel) {
      if (message.type === 'state') {
        // Keep only the newest state so a reconnect does not replay stale snapshots.
        this.queue = this.queue.filter((item) => item.type !== 'state');
      }
      this.queue.push(message);
      return false;
    }
    try {
      this.channel.send(message);
      return true;
    } catch {
      return false;
    }
  }
}
