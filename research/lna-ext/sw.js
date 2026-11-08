const ws = new WebSocket('ws://127.0.0.1:47800/');
ws.onopen = () => ws.send('hello from ' + chrome.runtime.id);
ws.onerror = e => console.log('err', e);
