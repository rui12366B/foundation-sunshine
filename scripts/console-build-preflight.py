#!/usr/bin/env python3
"""Verify fixed sources and driver assets before CMake; no drivers are installed."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]

def git(*args: str) -> str:
    return subprocess.check_output(['git', *args], cwd=ROOT, text=True).strip()

def prepare_vmouse(build: Path) -> dict:
    # Captured from the exact public v1.3.2 release, ID 334260567.
    # Do not rely on unauthenticated API rate limits at configure time.
    # Original FetchDriverDeps still rechecks each asset against this metadata.
    digests = {
        'ZakoVirtualMouse.cat': ('9e2118bc24f3d4d4382197d81a02c0924e85c7546385c8d678a4b5c025d2543c', 2494),
        'ZakoVirtualMouse.cer': ('5345f6ee35a9907c84e96e5f4575e8950a498513b35685f1811185f8b44868b7', 796),
        'ZakoVirtualMouse.dll': ('d85afe625e100e1d702737e153ad995a8cce21d5f264170d13781fb392e98ce4', 28584),
        'ZakoVirtualMouse.inf': ('0c42096ad1321ac6bc1c8b398ac3f6c3df990b1d7c4cccd9da3c1ec2c54e7fbd', 2121),
    }
    directory = build/'_driver_deps/vmouse'
    directory.mkdir(parents=True, exist_ok=True)
    base = 'https://github.com/AlkaidLab/zako-vmouse-release/releases/download/v1.3.2/'
    assets = []
    for name, (expected, size) in digests.items():
        target = directory/name
        failure = None
        for attempt in range(3):
            try:
                with urllib.request.urlopen(base+name, timeout=60) as response:
                    data = response.read(size+1)
                if len(data) != size or hashlib.sha256(data).hexdigest() != expected:
                    raise RuntimeError('Pinned vmouse file size or hash mismatch: '+name)
                target.write_bytes(data)
                failure = None
                break
            except Exception as error:
                failure = error
                if attempt < 2: time.sleep(2)
        if failure is not None: raise failure
        assets.append({'name':name, 'size':size, 'digest':'sha256:'+expected, 'browser_download_url':base+name})
    metadata = {'id':334260567, 'tag_name':'v1.3.2', 'assets':assets,
                'html_url':'https://github.com/AlkaidLab/zako-vmouse-release/releases/tag/v1.3.2'}
    (directory/'.release-metadata.json').write_text(json.dumps(metadata,indent=2),encoding='utf-8')
    (directory/'.release-version').write_text('AlkaidLab/zako-vmouse-release|v1.3.2\n',encoding='utf-8')
    return {'release':metadata['html_url'], 'assets':assets, 'status':'downloaded-and-sha256-verified; not installed'}

def main() -> None:
    build = ROOT/'build'
    build.mkdir(exist_ok=True)
    report = {'source_sha':git('rev-parse','HEAD'), 'dependencies':[], 'cmake_sources':{}}
    paths = ['third-party/build-deps','third-party/moonlight-common-c','third-party/googletest',
             'third-party/Simple-Web-Server','third-party/moonlight-audio-haptics']
    for rel in paths:
        entry = git('ls-tree','HEAD','--',rel).split()
        if len(entry) < 3 or entry[0] != '160000':
            raise RuntimeError('Not a pinned submodule: '+rel)
        expected = entry[2]
        folder = ROOT/rel
        if not folder.is_dir() or not (folder/'.git').exists():
            subprocess.run(['git','submodule','update','--init','--recursive','--',rel],cwd=ROOT,check=True)
        actual = git('-C',rel,'rev-parse','HEAD')
        if actual != expected: raise RuntimeError(f'{rel}: {actual} != {expected}')
        if rel.endswith('googletest') and not (folder/'CMakeLists.txt').is_file():
            subprocess.run(['git','-C',str(folder),'restore','--source='+expected,'--worktree','.'],check=True)
        report['dependencies'].append({'path':rel,'commit':actual,'cmake_exists':(folder/'CMakeLists.txt').is_file()})
    gtest = ROOT/'third-party/googletest'
    if not (gtest/'CMakeLists.txt').is_file() or not (gtest/'googletest/include/gtest/gtest.h').is_file():
        raise RuntimeError('The pinned GoogleTest checkout is incomplete; tests remain required')
    destination = build/'pinned-sources/googletest'
    if destination.exists(): raise RuntimeError('Refusing a stale isolated GoogleTest directory')
    destination.parent.mkdir(exist_ok=True)
    shutil.copytree(gtest,destination,ignore=shutil.ignore_patterns('.git'))
    for rel in ['cmake/dependencies/common.cmake','tests/CMakeLists.txt','CMakeLists.txt']:
        data = (ROOT/rel).read_bytes()
        report['cmake_sources'][rel] = {'sha256':hashlib.sha256(data).hexdigest(),
                                       'lines':len(data.splitlines()),'content':data.decode('utf-8')}
    redirects = build/'CMakeFiles/pkgRedirects'
    redirects.mkdir(parents=True,exist_ok=True)
    check = redirects/'write-probe'; check.write_text('ok'); check.unlink()
    report['vmouse'] = prepare_vmouse(build)
    (build/'source-preflight.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k != 'cmake_sources'},indent=2))

if __name__ == '__main__':
    main()
