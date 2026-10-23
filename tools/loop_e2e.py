#!/usr/bin/env python3
"""The test link end to end on this PC, without the game: the built loop_bot.exe against itself, then the built
StellarCoop.dll (loaded by loop_bot dll as the host, asked through coop-request.txt like Lua) against a bot guest. A twin
points the guest at a port nobody listens on and must fail. Windows only; build first (tools/install.py --dry-run).
Run: python tools/loop_e2e.py   Prints LOOP E2E: ok and exits 0 only when every check passes."""
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

BUILD = Path(__file__).resolve().parent.parent / 'build/windows'
BOT, DLL = BUILD / 'loop_bot.exe', BUILD / 'StellarCoop.dll'


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def pair(host_args, guest_port, guest_seconds=12):
    """(host output, guest output), both run at once."""
    host = subprocess.Popen([BOT, *host_args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    guest = subprocess.run([BOT, 'guest', str(guest_port), str(guest_seconds)], capture_output=True, text=True,
                           timeout=60)
    out, _ = host.communicate(timeout=60)
    return out, guest.stdout + guest.stderr


def result(text):
    found = re.findall(r'^BOT RESULT: (.*)$', text, re.M)
    assert found, text
    return found[-1]


def main():
    assert BOT.is_file() and DLL.is_file(), f'build first: {BOT}, {DLL}'
    # 1. Bot host and bot guest: linked, one chat line each way, and the guest's echo of the host's poses: every echoed
    # keyframe back exactly 150 cm along X (the host bot checks), none wrong.
    port = free_port()
    host, guest = pair(['host', str(port), '12'], port)
    fields = {side: dict(kv.split('=') for kv in result(text).split()) for side, text in (('host', host), ('guest', guest))}
    for side in fields:
        assert (fields[side]['linked'], fields[side]['said'], fields[side]['heard']) == ('1', '1', '1'), (side, fields)
    assert int(fields['guest']['echoed']) > 10 and int(fields['host']['echo_ok']) > 10, fields
    assert fields['host']['echo_bad'] == '0', fields
    # 2. The DLL hosts in its own folder; the bot joins it.
    with tempfile.TemporaryDirectory() as tmp:
        shutil.copy2(DLL, tmp)
        port = free_port()
        dll, guest = pair(['dll', tmp, str(port), '25'], port)
        assert result(dll) == 'dll linked=1', dll
        assert 'TEST LINK on: loopback UDP as host' in dll and 'never Steam' in dll, dll
        assert result(guest).startswith('linked=1'), guest
    # 3b. The host's world: the bot hosts with a save (EVAS, GVAS, CRC-32, 650 KB: 11 chunks), the DLL joins as guest in
    # a fresh saves folder, asks for it, and must write exactly those bytes into all five of its host files.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        b = b'EVAS\x01\x00\x00\x00GVAS' + os.urandom(650000)
        world = tmp / 'world.sav'
        world.write_bytes(b + struct.pack('<I', zlib.crc32(b[8:]) & 0xffffffff))
        (tmp / 'g').mkdir()
        shutil.copy2(DLL, tmp / 'g')
        port = free_port()
        host = subprocess.Popen([BOT, 'host', str(port), '40', '0', '0', world], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        guest = subprocess.run([BOT, 'dllguest', tmp / 'g', str(port), '40', world], capture_output=True, text=True,
                               timeout=90)
        host.kill()
        host_out, _ = host.communicate(timeout=30)
        assert 'in 11 chunks' in host_out, host_out
        assert result(guest.stdout) == 'dll guest linked=1 world_written=1 same_bytes=1 bytes=650016', guest.stdout
        assert "host's world received: 650016 bytes" in guest.stdout, guest.stdout
    # 3. Twin: the guest knocks on a port nobody listens on, so nothing may report linked.
    host_port, wrong = free_port(), free_port()
    host, guest = pair(['host', str(host_port), '8'], wrong, 8)
    assert result(host).startswith('linked=0') and result(guest).startswith('linked=0'), (host, guest)
    print('LOOP E2E: ok')


if __name__ == '__main__':
    sys.exit(main())
