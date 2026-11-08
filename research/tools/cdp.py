import json, sys, time, urllib.request, websocket
port, ext = sys.argv[1], sys.argv[2]
for _ in range(50):
    try: v = json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json/version')); break
    except Exception: time.sleep(0.3)
ws = websocket.create_connection(v['webSocketDebuggerUrl'])
ws.send(json.dumps({'id':1,'method':'Extensions.loadUnpacked','params':{'path':ext}}))
print('CDP', ws.recv())
