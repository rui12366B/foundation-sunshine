#!/usr/bin/env python3
"""Verify the pinned sources immediately before CMake and preserve diagnostics.

This runs in the dedicated CI checkout, never the installed Sunshine directory.
It does not turn off tests or silently substitute a different dependency version.
"""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

def git(*args: str) -> str:
    return subprocess.check_output(['git', *args], cwd=ROOT, text=True).strip()

def main() -> None:
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    report = {'source_sha': git('rev-parse', 'HEAD'), 'dependencies': [], 'cmake_sources': {}}
    paths = ['third-party/build-deps', 'third-party/moonlight-common-c', 'third-party/googletest',
             'third-party/Simple-Web-Server', 'third-party/moonlight-audio-haptics']
    for rel in paths:
        entry = git('ls-tree', 'HEAD', '--', rel).split()
        if len(entry) < 3 or entry[0] != '160000':
            raise RuntimeError('Not a pinned submodule: ' + rel)
        expected = entry[2]
        folder = ROOT / rel
        if not folder.is_dir() or not (folder / '.git').exists():
            subprocess.run(['git', 'submodule', 'update', '--init', '--recursive', '--', rel], cwd=ROOT, check=True)
        actual = git('-C', rel, 'rev-parse', 'HEAD')
        if actual != expected:
            raise RuntimeError(f'{rel}: {actual} != {expected}')
        if rel.endswith('googletest') and not (folder/'CMakeLists.txt').is_file():
            # Restore only the fixed dependency in this disposable build checkout.
            subprocess.run(['git','-C',str(folder),'restore','--source='+expected,'--worktree','.'],check=True)
        report['dependencies'].append({'path':rel,'commit':actual,'cmake_exists':(folder/'CMakeLists.txt').is_file()})
    gtest = ROOT/'third-party/googletest'
    if not (gtest/'CMakeLists.txt').is_file() or not (gtest/'googletest/include/gtest/gtest.h').is_file():
        raise RuntimeError('The pinned GoogleTest checkout is incomplete; tests remain required')
    destination = build/'pinned-sources/googletest'
    if destination.exists():
        raise RuntimeError('The isolated GoogleTest copy already exists; do not reuse stale configure state')
    destination.parent.mkdir(exist_ok=True)
    shutil.copytree(gtest, destination, ignore=shutil.ignore_patterns('.git'))
    for rel in ['cmake/dependencies/common.cmake','tests/CMakeLists.txt','CMakeLists.txt']:
        data = (ROOT/rel).read_bytes()
        report['cmake_sources'][rel] = {'sha256':hashlib.sha256(data).hexdigest(),
                                       'lines':len(data.splitlines()),'content':data.decode('utf-8')}
    redirects=build/'CMakeFiles/pkgRedirects'
    redirects.mkdir(parents=True,exist_ok=True)
    check=redirects/'write-probe'; check.write_text('ok'); check.unlink()
    (build/'source-preflight.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='cmake_sources'},indent=2))

if __name__=='__main__':
    main()
