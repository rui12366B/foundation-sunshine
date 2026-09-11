#!/usr/bin/env python3
"""Prepare test-build assets without font binaries or changes to signed drivers."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import re
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FONTS = {'.ttf','.otf','.woff','.woff2','.ttc','.otc','.eot','.dfont','.pfa','.pfb','.fon','.fnt','.bdf','.pcf'}
RELEASE = 'https://github.com/AlkaidLab/foundation-sunshine/releases/download/v2026.911.85650.%E6%9D%82%E9%B1%BC/Sunshine.v2026.0911.Portable-x64.zip'
ARCHIVE_SHA = '1da04ac3a114199018f39636ed3a386b3ab57f59968400856bf582fca1425743'
ADAPTER_SHA = 'bffcf3ba0586d8ed6ffde6574689157cb291163c010c5bff4b96052710060d74'
RUNTIME_SHA = '9a80575f247190c05fe80eac0c4baa1d0d4d932348f26808310b5ec4bf9eeb4b'

def strip_font_assets(folder: Path) -> int:
    removed = 0
    for path in folder.rglob('*'):
        if '.git' in path.parts or path.is_symlink() or not path.is_file():
            continue
        if path.suffix.lower() in FONTS:
            path.unlink(); removed += 1
        elif path.suffix.lower() in {'.css','.html','.vue','.less'}:
            text = path.read_text(encoding='utf-8')
            clean = re.sub(r'@font-face\s*\{[^}]*\}', '', text, flags=re.S)
            if text != clean:
                path.write_text(clean, encoding='utf-8', newline='\n')
    return removed

def main() -> None:
    out = ROOT/'build/pinned-rtx'; out.mkdir(parents=True, exist_ok=True)
    archive = out/'official-portable.zip'
    urllib.request.urlretrieve(RELEASE, archive)
    with archive.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != ARCHIVE_SHA:
            raise RuntimeError('Official release archive failed its pinned SHA-256 check')
    with zipfile.ZipFile(archive) as z:
        matches = {Path(name).name:name for name in z.namelist() if Path(name).name in {'sunshine.exe','foundation_rtx_video_adapter.dll'}}
        if len(matches) != 2:
            raise RuntimeError('Pinned official bundle is incomplete')
        original = z.read(matches['sunshine.exe'])
        adapter = z.read(matches['foundation_rtx_video_adapter.dll'])
        if hashlib.sha256(adapter).hexdigest() != ADAPTER_SHA:
            raise RuntimeError('First-party adapter digest mismatch')
        values = set(m.group().decode() for m in re.finditer(rb'(?<![a-f0-9])[a-f0-9]{64}(?![a-f0-9])', original))
        if values != {ADAPTER_SHA,RUNTIME_SHA}:
            raise RuntimeError('The original binary does not contain exactly the expected adapter/runtime trust roots')
        (out/'foundation_rtx_video_adapter.dll').write_bytes(adapter)
    archive.unlink()
    # Use SVG Font Awesome icons rather than distributing the icon font.
    header = ROOT/'src_assets/common/assets/web/template_header.html'
    text = header.read_text(encoding='utf-8')
    old = '<link href="@fortawesome/fontawesome-free/css/all.min.css" rel="stylesheet">'
    new = '<script type="module" src="@fortawesome/fontawesome-free/js/all.min.js"></script>'
    if old not in text and new not in text:
        raise RuntimeError('Font Awesome asset marker changed; do not package unresolved font references')
    header.write_text(text.replace(old,new),encoding='utf-8',newline='\n')
    count = strip_font_assets(ROOT/'src_assets')
    info = {'type':'experimental console display helper build','pinned_original_release':RELEASE,
            'original_archive_sha256':ARCHIVE_SHA,'adapter_sha256':ADAPTER_SHA,
            'runtime_sha256':RUNTIME_SHA,'rtx_hdr':'enabled, unchanged first-party adapter; runtime imported as in upstream',
            'fonts':'system fonts and SVG icons; font binaries excluded','font_files_removed':count,
            'runtime_validation':'RDP/Zako/RTX 3080 scenarios not verified by build runner'}
    (ROOT/'build/test-build-provenance.json').write_text(json.dumps(info,indent=2),encoding='utf-8')
    print(json.dumps(info,indent=2))

if __name__ == '__main__':
    main()
