#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Convert the pinned SDK's host kernels; optionally compile them with Apple Metal.

All generated SPIR-V/MSL/metallibs stay in the selected output directory. The
report distinguishes successful conversion from unsupported source interfaces.
It never marks the latter as passing rendered pixels.
"""
import argparse, json, re, struct, subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--sdk', type=Path, required=True)
p.add_argument('--exporter', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--apple-sdk', choices=['macosx', 'iphoneos'])
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
results = []
for header in sorted((a.sdk / 'src/graphics/shaders/vulkan_spirv').glob('*.h')):
    text = header.read_text(encoding='utf-8')
    data = text.split('#endif', 1)[1]
    values = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', data)]
    if not values or values[0] != 0x07230203:
        raise RuntimeError(f'No SPIR-V module in {header.name}')
    name = header.stem
    spv, msl, meta = [a.output / (name + ext) for ext in ('.spv', '.metal', '.json')]
    spv.write_bytes(struct.pack('<' + 'I' * len(values), *values))
    stage = 'vertex' if name.endswith('_vs') else 'fragment' if name.endswith('_ps') else 'compute'
    run = subprocess.run([str(a.exporter.resolve()), stage, str(spv), str(msl), str(meta)], capture_output=True, text=True)
    item = {'name': name, 'conversion': 'passed' if run.returncode == 0 else 'unsupported', 'detail': (run.stdout + run.stderr).strip()}
    if run.returncode not in (0, 1):
        raise RuntimeError(f'Exporter failure: {item}')
    if run.returncode == 0 and a.apple_sdk:
        air = a.output / (name + '.air')
        command = ['xcrun', '--sdk', a.apple_sdk, 'metal', '-std=metal2.3', '-fno-fast-math', '-c', str(msl), '-o', str(air)]
        if a.apple_sdk == 'iphoneos':
            command.insert(4, '-mios-version-min=18.0')
        compile_result = subprocess.run(command, capture_output=True, text=True)
        item['metal_compile'] = 'passed' if compile_result.returncode == 0 else 'failed'
        item['metal_detail'] = (compile_result.stdout + compile_result.stderr).strip()
    results.append(item)
    print(f"{name}: {item['conversion']}" + (f" / Metal {item['metal_compile']}" if 'metal_compile' in item else ''), flush=True)
report = {'modules': results, 'converted': sum(r['conversion'] == 'passed' for r in results),
          'unsupported': sum(r['conversion'] == 'unsupported' for r in results),
          'metal_failures': sum(r.get('metal_compile') == 'failed' for r in results)}
(a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps({k: v for k, v in report.items() if k != 'modules'}))
if report['metal_failures']:
    raise SystemExit(1)
# These kernels are required by the first frontend milestone. Other unsupported
# kernels remain visible in the report until their texture interfaces are lowered.
required = {'fullscreen_cw_vs', 'apply_gamma_pwl_ps', 'apply_gamma_table_ps',
            'texture_load_32bpb_cs', 'resolve_clear_32bpp_cs'}
by_name = {r['name']: r for r in results}
for name in required:
    if name not in by_name or by_name[name]['conversion'] != 'passed':
        raise SystemExit(f'Required native kernel is unavailable: {name}')
