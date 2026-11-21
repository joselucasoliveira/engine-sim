#!/usr/bin/env python3
"""Rebuild and verify the checked SDL SPIR-V in an explicit external directory."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'assets/shaders/sdl'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--glslang', default=shutil.which('glslang') or 'glslangValidator')
    parser.add_argument('--spirv-val', default='spirv-val')
    parser.add_argument('--spirv-dis', default='spirv-dis')
    options = parser.parse_args()
    output = options.out.resolve()
    if not options.out.is_absolute() or output.is_relative_to(ROOT):
        raise ValueError('Choose an absolute shader output directory outside the checkout')
    output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((SOURCE / 'manifest.json').read_text())
    for name, expected in manifest['files'].items():
        if sha(SOURCE / name) != expected:
            raise ValueError(f'Checked shader input differs: {name}')
    commands = []
    def run(argv):
        result = subprocess.run(argv, text=True, capture_output=True)
        commands.append(dict(argv=argv, exit=result.returncode, stdout=result.stdout, stderr=result.stderr))
        (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        if result.returncode:
            raise ValueError(f'Shader command failed: {argv[0]}: {result.stdout}{result.stderr}')
        return result.stdout
    for tool in (options.glslang, options.spirv_val, options.spirv_dis):
        run([tool, '--version'])
    for stage, entry, suffix in [('vert', 'VSMain', 'vertex'), ('frag', 'PSMain', 'fragment')]:
        name = f'engine_sim.{suffix}.spv'
        destination = output / name
        run([options.glslang, '-D', '-V', '--target-env', 'vulkan1.0', '-S', stage,
             '-e', entry, '--hlsl-iomap', str(SOURCE / 'engine_sim.hlsl'), '-o', str(destination)])
        run([options.spirv_val, '--target-env', 'vulkan1.0', str(destination)])
        assembly = run([options.spirv_dis, str(destination)])
        (output / (name + '.asm')).write_text(assembly)
        if f'"{entry}"' not in assembly or 'Binding 0' not in assembly:
            raise ValueError(f'Missing entry point or uniform binding: {name}')
        if sha(destination) != manifest['files'][name]:
            raise ValueError(f'Regenerated artifact differs: {name}; inspect external output and tool versions')
    print('Both validated SPIR-V artifacts match the checked shader manifest')

if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
