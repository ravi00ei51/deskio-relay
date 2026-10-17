#!/usr/bin/env python3
"""Interactive registration/login and authenticated relay test.
Install: python -m pip install websockets==15.0.1
"""
import argparse
import asyncio
import getpass
import json
import sys
from websockets.asyncio.client import connect

async def run(args):
    username = input('Username: ').strip()
    password = getpass.getpass('Password: ')
    payload = {'username': username, 'password': password}
    if args.register:
        payload.update({'type': 'register', 'name': input('Full name: '),
                        'dob': input('DOB (YYYY-MM-DD): '),
                        'address': input('Address: ')})
    async with connect(args.url, open_timeout=90, ping_interval=20,
                       max_size=8 * 1024 * 1024) as ws:
        await ws.send(json.dumps(payload))
        del payload, password
        response = json.loads(await asyncio.wait_for(ws.recv(), timeout=65))
        print('Server:', json.dumps(response))
        if response.get('type') != 'auth.success':
            return 1
        print('Authenticated. Starting text/binary relay probe for 30 seconds.')
        if not response.get('relay_available'):
            print('No relay slot: disconnect another connection for this account and reconnect.')
            return 1
        async def send():
            sequence = 0
            while True:
                sequence += 1
                await ws.send(json.dumps({'from': args.label, 'sequence': sequence,
                                          'message': 'Hello from authenticated client'}))
                await ws.send(f'{args.label}: binary probe {sequence}'.encode())
                await asyncio.sleep(2)
        async def receive():
            async for data in ws:
                print('Peer:', repr(data))
        tasks = [asyncio.create_task(send()), asyncio.create_task(receive())]
        try:
            done, _ = await asyncio.wait(tasks, timeout=30,
                                         return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
    return 0

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='ws://localhost:10000/ws')
    parser.add_argument('--register', action='store_true')
    parser.add_argument('--label', default='computer1')
    args = parser.parse_args()
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130
    except Exception as error:
        print('Test failed:', error, file=sys.stderr)
        return 1

if __name__ == '__main__':
    sys.exit(main())
