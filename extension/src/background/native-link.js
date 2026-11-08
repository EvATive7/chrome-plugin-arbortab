// WebSocket client for the ArborTab native module.

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

export class NativeLink {
  constructor({ onMessage, onStatus, onHello } = {}) {
    this.onMessage = onMessage || (() => {});
    this.onStatus = onStatus || (() => {});
    this.onHello = onHello || (() => {});
    this.socket = null;
    this.port = null;
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
    this.stopped = false;
  }

  get isConnected() {
    return this.connected;
  }

  start() {
    this.stopped = false;
    this.connect();
  }

  stop() {
    this.stopped = true;
    if (this.heartbeat) {
      clearInterval(this.heartbeat);
      this.heartbeat = null;
    }
    if (this.socket) {
      try {
        this.socket.close();
      } catch {
        // ignore
      }
      this.socket = null;
    }
    this.connected = false;
    this.onStatus({ connected: false, features: this.features });
  }

  connect() {
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
    this.socket = socket;

    socket.addEventListener('open', () => {
      this.connected = true;
      this.port = port;
      this.retryDelay = 500;
      this.portIndex = 0;
      this.send({ type: 'hello', protocol: PROTOCOL, extVersion: chrome.runtime.getManifest().version });
      for (const message of this.queue.splice(0)) {
        this.send(message);
      }
      this.heartbeat = setInterval(() => this.send({ type: 'ping' }), 20000);
      this.onStatus({ connected: true, features: this.features });
    });

    socket.addEventListener('message', (event) => {
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        return;
      }
      if (message.type === 'hello' || message.type === 'status') {
        this.updateNeeded = protocolMismatch(message);
        this.native = {
          nativeVersion: message.nativeVersion,
          offsets: message.offsets || null,
          protocolMin: message.protocolMin ?? message.protocol,
          protocolMax: message.protocolMax ?? message.protocol,
        };
        this.features = this.updateNeeded ? { ...NO_FEATURES } : { ...NO_FEATURES, ...message.features };
        if (message.type === 'hello' && !this.updateNeeded) {
          this.onHello(message);
        }
        this.onStatus({ connected: true, features: this.features, nativeVersion: message.nativeVersion });
        return;
      }
      if (!this.updateNeeded) {
        this.onMessage(message);
      }
    });

    const onDown = () => {
      if (!this.connected && this.socket !== socket) {
        return;
      }
      this.connected = false;
      this.socket = null;
      if (this.heartbeat) {
        clearInterval(this.heartbeat);
        this.heartbeat = null;
      }
      // Try the next port on reconnect: the browser may have moved to another one.
      this.portIndex += 1;
      this.onStatus({ connected: false, features: this.features });
      this.scheduleReconnect();
    };
    socket.addEventListener('close', onDown);
    socket.addEventListener('error', onDown);
  }

  scheduleReconnect() {
    if (this.stopped) {
      return;
    }
    const delay = this.retryDelay;
    this.retryDelay = Math.min(this.retryDelay * 2, 30000);
    setTimeout(() => this.connect(), delay);
  }

  send(message) {
    if (this.updateNeeded && message.type !== 'hello' && message.type !== 'ping') {
      return false;  // an incompatible module must not receive messages it would misread
    }
    const payload = typeof message === 'string' ? message : JSON.stringify(message);
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
      if (message.type === 'state') {
        // Keep only the newest state so a reconnect does not replay stale snapshots.
        this.queue = this.queue.filter((item) => item.type !== 'state');
      }
      this.queue.push(typeof message === 'string' ? JSON.parse(message) : message);
      return false;
    }
    this.socket.send(payload);
    return true;
  }
}
