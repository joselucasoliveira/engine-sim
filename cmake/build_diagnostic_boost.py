"""Build the pinned Boost runtime archives with the selected diagnostic compiler."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--sanitizer', choices=('address', 'thread'), required=True)
    options = parser.parse_args()
    root, out, compiler = options.root.resolve(), options.out.resolve(), options.compiler.absolute()
    if not (root / 'boost/version.hpp').is_file() or not (root / 'b2').is_file():
        parser.error('require the verified bootstrap Boost source and b2')
    if out.is_relative_to(root) or out.is_relative_to(Path(__file__).resolve().parents[1]):
        parser.error('diagnostic build must remain outside source inputs and product')
    out.mkdir(parents=True, exist_ok=True)
    kind = 'clang' if 'clang' in compiler.name else 'gcc'
    config = out / 'user-config.jam'
    config.write_text(f'using {kind} : diagnostic : "{compiler.as_posix()}" ;\n')
    runtime = 'address,undefined' if options.sanitizer == 'address' else 'thread'
    flags = f'-fPIC -g -O1 -fno-omit-frame-pointer -fsanitize={runtime}'
    if options.sanitizer == 'address':
        flags += ' -fno-sanitize-recover=all'
    argv = [str(root / 'b2'), '--with-filesystem', '--user-config=' + str(config),
            '--build-dir=' + str(out / 'objects'), '--stagedir=' + str(out / 'stage'),
            '--layout=system', 'toolset=' + kind + '-diagnostic', 'variant=release',
            'link=static', 'runtime-link=shared', 'threading=multi', 'cxxstd=17',
            'cxxflags=' + flags, 'linkflags=-fsanitize=' + runtime, '-j3', 'stage']
    attempt = 1
    while (out / f'build-{attempt}.log').exists():
        attempt += 1
    logfile = out / f'build-{attempt}.log'
    with logfile.open('xb') as log:
        result = subprocess.run(argv, cwd=root, stdout=log, stderr=subprocess.STDOUT)
    archives = [out / 'stage/lib' / ('libboost_' + name + '.a') for name in ('filesystem', 'atomic')]
    record = {'argv': argv, 'cwd': str(root), 'compiler': str(compiler), 'sanitizer': runtime,
              'exit': result.returncode, 'log_sha256': hashlib.sha256(logfile.read_bytes()).hexdigest(),
              'archives': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in archives if p.is_file()}}
    (out / f'receipt-{attempt}.json').write_text(json.dumps(record, indent=2) + '\n')
    if result.returncode or len(record['archives']) != len(archives):
        raise RuntimeError(f'diagnostic Boost build failed: {logfile}')
    print(f'Instrumented Boost filesystem/atomic: {runtime}')


if __name__ == '__main__':
    main()
