#!/usr/bin/env python3
"""Apply the reviewed, content-addressed integration to its exact source baseline.

Every original and result is SHA-256 checked before any write. Changes are
materialized as normal C++/CMake files and committed by the build workflow.
No system settings, credentials, running service, or drivers are accessed.
"""
from __future__ import annotations
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main() -> None:
    spec = json.loads((ROOT / 'port/console-helper-integration.json').read_text(encoding='utf-8'))
    pending = []
    for item in spec['files']:
        rel = Path(item['path'])
        if rel.is_absolute() or '..' in rel.parts:
            raise RuntimeError('Unsafe patch path')
        path = ROOT / rel
        if path.is_symlink():
            raise RuntimeError(f'Refusing symlink: {rel}')
        data = path.read_bytes().replace(b'\r\n', b'\n')
        digest = hashlib.sha256(data).hexdigest()
        if digest == item['new_sha256']:
            print(f'Already applied: {rel}')
            continue
        if digest != item['old_sha256']:
            raise RuntimeError(f'Original source has changed: {rel}; reconcile instead of overwriting')
        lines = data.decode('utf-8').splitlines(keepends=True)
        previous = len(lines) + 1
        for start, end, replacement in reversed(item['edits']):
            if not 0 <= start <= end <= len(lines) or end > previous:
                raise RuntimeError(f'Invalid/overlapping edit for {rel}')
            lines[start:end] = [replacement]
            previous = start
        result = ''.join(lines).encode('utf-8')
        if hashlib.sha256(result).hexdigest() != item['new_sha256']:
            raise RuntimeError(f'Patched result digest mismatch: {rel}')
        pending.append((path, result))
    # This separate include leaves the original SDK build path available.
    path = ROOT / 'cmake/dependencies/rtx_video_adapter.cmake'
    data = path.read_bytes().replace(b'\r\n', b'\n')
    marker = b'include("${CMAKE_CURRENT_LIST_DIR}/FetchRtxVideoSdk.cmake")'
    addition = (b'# Optional exact-release first-party adapter, with unchanged runtime verification.\n'
                b'if (SUNSHINE_PINNED_RELEASE_ADAPTER_DIR)\n'
                b'    include("${CMAKE_CURRENT_LIST_DIR}/PinnedOfficialRtxAdapter.cmake")\n'
                b'    return()\nendif ()\n\n')
    if b'PinnedOfficialRtxAdapter.cmake' not in data:
        if data.count(marker) != 1:
            raise RuntimeError('RTX CMake integration marker mismatch')
        pending.append((path, data.replace(marker, addition + marker)))
    path = ROOT / 'src/platform/windows/display_session_bridge/win32_support.h'
    data = path.read_bytes().replace(b'\r\n', b'\n')
    if b'#define WIN32_LEAN_AND_MEAN' not in data:
        if data.count(b'#include <windows.h>') != 1:
            raise RuntimeError('Win32 support marker mismatch')
        pending.append((path, data.replace(b'#include <windows.h>', b'#ifndef WIN32_LEAN_AND_MEAN\n#define WIN32_LEAN_AND_MEAN\n#endif\n#include <windows.h>')))
    for path, data in pending:
        path.write_bytes(data)
        print(f'Applied: {path.relative_to(ROOT)}')
    print(f'Content-verified integration complete; {len(pending)} files updated')

if __name__ == '__main__':
    main()
