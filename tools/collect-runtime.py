from pathlib import Path
import hashlib, json, shutil, struct, subprocess, sys

sdk, build, output = [Path(p).resolve() for p in sys.argv[1:4]]
output.mkdir(exist_ok=False)
records = []
for name in ('librexruntime.dylib', 'librexgpu-xenos.dylib', 'libMoltenVK.dylib'):
    # The pinned SDK sets its shared-library output beneath the source tree.
    matches = sorted({p.resolve() for root in (build, sdk / 'out' / 'mac-arm64')
                      for p in root.rglob(name)})
    assert len(matches) == 1, (name, matches)
    path = matches[0]
    data = path.read_bytes()
    assert struct.unpack_from('<II', data) == (0xfeedfacf, 0x100000c), name
    assert struct.unpack_from('<I', data, 12)[0] == 6, name
    position = 32
    versions = []
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, position)
        assert size >= 8 and position + size <= len(data)
        if command == 0x32: versions.append(struct.unpack_from('<II', data, position + 8))
        position += size
    assert versions == [(2, 0x120000)], (name, versions)
    arch = subprocess.check_output(['xcrun', 'lipo', '-archs', str(path)], text=True).strip()
    assert arch == 'arm64', (name, arch)
    load = subprocess.check_output(['xcrun', 'otool', '-l', str(path)], text=True)
    deps = subprocess.check_output(['xcrun', 'otool', '-L', str(path)], text=True)
    shutil.copyfile(path, output / name)
    (output / (name + '.load-commands.txt')).write_text(load + '\n' + deps, encoding='utf-8')
    records.append(dict(name=name, sha256=hashlib.sha256(path.read_bytes()).hexdigest(), bytes=path.stat().st_size))
for path in sdk.rglob('*'):
    relative = path.relative_to(sdk)
    if not path.is_file() or '.git' in relative.parts: continue
    if not any(token in path.name.upper() for token in ('LICENSE', 'COPYING', 'NOTICE')): continue
    if path.suffix.lower() not in ('', '.txt', '.md', '.rst', '.html'): continue
    dest = output / 'Acknowledgements' / relative
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(path, dest)
manifest = dict(sdk_commit=subprocess.check_output(['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True).strip(),
                files=records, device_execution_verified=False,
                input_manifest=json.loads((Path(__file__).resolve().parent.parent / 'INPUTS.json').read_text()),
                xcode=subprocess.check_output(['xcodebuild', '-version'], text=True),
                ios_sdk=subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-version'], text=True))
(output / 'BUILD-MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
print(json.dumps(records, indent=2))
