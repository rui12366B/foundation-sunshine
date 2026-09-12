#!/usr/bin/env python3
"""Materialize the reviewed 503 follow-up against its verified source hashes.

The earlier helper migration is already committed. Do not replay that historical
migration on top of follow-up fixes. This step writes only checked source files
in a disposable build checkout, never a running host's config or driver state.
"""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main():
    spec = json.loads((ROOT / 'port/console-primary-503.json').read_text(encoding='utf-8'))
    pending = []
    for entry in spec['files']:
        rel = Path(entry['path'])
        if rel.is_absolute() or '..' in rel.parts:
            raise RuntimeError('Unsafe patch path')
        path = ROOT / rel
        if path.is_symlink():
            raise RuntimeError('Refusing symlink: ' + str(rel))
        old = path.read_bytes().replace(b'\r\n', b'\n')
        digest = hashlib.sha256(old).hexdigest()
        if digest == entry['new_sha256']:
            continue
        if digest != entry['old_sha256']:
            raise RuntimeError('Source changed; reconcile instead of overwriting: ' + str(rel))
        lines = old.decode('utf-8').splitlines(keepends=True)
        previous = len(lines) + 1
        for start, end, replacement in reversed(entry['edits']):
            if not 0 <= start <= end <= len(lines) or end > previous:
                raise RuntimeError('Invalid or overlapping edit')
            lines[start:end] = [replacement]
            previous = start
        new = ''.join(lines).encode('utf-8')
        if hashlib.sha256(new).hexdigest() != entry['new_sha256']:
            raise RuntimeError('Result digest mismatch: ' + str(rel))
        pending.append((path, new))
    for path, data in pending:
        path.write_bytes(data)
        print('Updated:', path.relative_to(ROOT))
    # Enforce essential wiring before spending time on a Windows build.
    session = (ROOT / 'src/display_device/session.cpp').read_text(encoding='utf-8')
    assert session.index('settings.apply_config(*parsed_config') < session.index('detail::reconcile_primary(')
    assert 'reverting failed launch before capture' in session
    assert 'Primary verified after modes/HDR' in session
    assert 'state_retry_timer' in session
    print('503 follow-up checked; normal failure rollback remains enabled')

if __name__ == '__main__':
    main()
