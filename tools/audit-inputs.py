from pathlib import Path
import hashlib, json

root = Path(__file__).resolve().parent.parent
manifest = json.loads((root / 'INPUTS.json').read_text(encoding='utf-8'))
assert manifest['sdk_commit'] == 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518'
expected = manifest['files']
actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()
          and '.git' not in p.relative_to(root).parts and '__pycache__' not in p.parts}
# Generated build/clone directories are absent at the workflow's first step.
assert actual == set(expected) | {'INPUTS.json'}, ('Unexpected publication input', actual ^ (set(expected) | {'INPUTS.json'}))
for name, digest in expected.items():
    path = (root / name).resolve(strict=True)
    assert path.is_relative_to(root)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, name
    assert path.suffix.lower() not in ('.ipa', '.xex', '.iso', '.csv', '.dylib')
print(f'{len(expected)} reviewed source/license/workflow files verified; no game or binary input')
