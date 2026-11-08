import asyncio, websockets, sys
async def h(ws):
    print('ORIGIN', ws.request.headers.get('Origin'), flush=True)
    print('MSG', await ws.recv(), flush=True)
async def main():
    async with websockets.serve(h, '127.0.0.1', 47800):
        await asyncio.sleep(40)
asyncio.run(main())
