"""Move an SDK and build/run independent public-header consumers on its native host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

PUBLIC = {'es_api_version','es_capabilities','es_config_init','es_create','es_load','es_set_controls','es_render','es_get_telemetry','es_reset','es_unload','es_destroy'}

def file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()

def command(argv, cwd, report, env=None):
    try:
        result = subprocess.run(list(map(str, argv)), cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
        code, raw = result.returncode, result.stdout
    except subprocess.TimeoutExpired as error:
        code, raw = None, error.stdout or b''
    except OSError as error:
        code, raw = None, str(error).encode('utf-8')
    output = raw.decode('utf-8',errors='replace')
    report.append({'argv':list(map(str, argv)), 'cwd':str(cwd), 'exit':code, 'output':output,
                   'stdout_sha256':hashlib.sha256(raw).hexdigest()})
    logs = cwd / 'commands'; logs.mkdir(exist_ok=True)
    (logs / ('%03d.json' % len(report))).write_text(json.dumps(report[-1],indent=2)+'\n',encoding='utf-8')
    (logs / ('%03d.log' % len(report))).write_bytes(raw)
    if code != 0:
        raise RuntimeError(output or 'Consumer command timed out')
    return output

def executable(build, name):
    """Support single- and multi-config generators on every native host."""
    filename = name + ('.exe' if platform.system() == 'Windows' else '')
    matches = [path for path in (build / filename, build / 'Release' / filename) if path.is_file()]
    if len(matches) != 1:
        raise RuntimeError('Expected exactly one built executable: ' + filename)
    return matches[0]

def inside(prefix, fixture, work, variant, report):
    cmake = shutil.which('cmake')
    build = work/'consumer-build'
    command([cmake,'-S',work/'consumer-source','-B',build,'-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_PREFIX_PATH={prefix}',
             '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF','-DENGINE_SIM_CONSUMER_INSPECT=ON'],work,report)
    command([cmake,'--build',build,'--config','Release','--parallel','2','--verbose'],work,report)
    env = os.environ.copy()
    properties = (build/'imported-properties.txt').read_text(encoding='utf-8')
    report.append({'imported_properties':properties})
    for line in properties.splitlines():
        if '.INTERFACE_INCLUDE_DIRECTORIES=' in line:
            value=line.split('=',1)[1]
            if line.startswith('EngineSim::Runtime.'):
                assert Path(value).resolve() == (prefix/'include').resolve(),line
            else:
                assert value == 'value-NOTFOUND',line
        if '.IMPORTED_LOCATION=' in line or '.IMPORTED_IMPLIB=' in line:
            value=line.split('=',1)[1]
            if value.endswith('-NOTFOUND'): continue
            assert Path(value).is_file() and Path(value).is_relative_to(prefix),line
            if Path(value).suffix in ('.a','.lib'):
                with Path(value).open('rb') as stream:
                    assert stream.read(8) != b'!<thin>\n','Thin archives cannot be relocated independently'
    assert ('ES_RUNTIME_STATIC' in properties) == (variant == 'static')
    location=Path(next(line.split('=',1)[1] for line in properties.splitlines() if line.startswith('EngineSim::Runtime.IMPORTED_LOCATION=')))
    if platform.system() == 'Windows':
        env['PATH'] = str(location.parent)+os.pathsep+env.get('PATH','')
    extension = '.exe' if platform.system()=='Windows' else ''
    for name in ('runtime-c-consumer','runtime-cxx-consumer'):
        command([executable(build,name),fixture/'main.mr',fixture/'library',fixture],work,report,env)
    matrix_build = work / 'matrix-build'
    command([cmake,'-S',work/'matrix-source','-B',matrix_build,'-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_PREFIX_PATH={prefix}',
             '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF'],work,report)
    command([cmake,'--build',matrix_build,'--config','Release','--parallel','2','--verbose'],work,report)
    matrices = []
    for name in ('runtime-matrix-c','runtime-matrix-cxx'):
        output = command([executable(matrix_build,name),fixture/'main.mr',fixture/'library',fixture],work,report,env)
        rows = [json.loads(line) for line in output.splitlines()]
        assert {(row['rate'],row['mode']) for row in rows} == {(rate,mode) for rate in (44100,48000) for mode in (0,1)}
        assert len(rows) == 4
        matrices.append(rows)
    keys = ('rate','mode','frames','ticks','native_frames','pcm_fnv1a')
    assert [{key:row[key] for key in keys} for row in matrices[0]] == [{key:row[key] for key in keys} for row in matrices[1]]
    report.append({'native_matrix':matrices,'fingerprint':'FNV-1a over exact PCM bytes; diagnostic identity, not an authenticity certificate'})
    if variant == 'shared':
        # The transferred helper imports only the standalone public ctypes code.
        sys.path.insert(0,str(work))
        try:
            from ffi_matrix import matrix
            foreign = matrix(location,fixture)
        finally:
            sys.path.pop(0)
        assert foreign == [{key:row[key] for key in keys} for row in matrices[0]]
        report.append({'foreign_matrix':foreign})
    offline=list(prefix.rglob('engine-sim-render'+extension))
    if offline:
        companion=list(prefix.rglob('engine-sim-runtime-consumer'+extension))
        assert len(companion)==1
        command([companion[0],fixture/'main.mr',fixture/'library',fixture],work,report,env)
        offline_build=work/'offline-build'
        command([cmake,'-S',work/'offline-source','-B',offline_build,'-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_PREFIX_PATH={prefix}',
                 '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF'],work,report)
        command([cmake,'--build',offline_build,'--config','Release','--parallel','2'],work,report)
        reference_build=work/'reference-build'
        command([cmake,'-S',work/'reference-source','-B',reference_build,'-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_PREFIX_PATH={prefix}',
                 '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF'],work,report)
        command([cmake,'--build',reference_build,'--config','Release','--parallel','2'],work,report)
        reference=executable(reference_build,'runtime-direct-reference')
        arguments=[sys.executable,'-B',work/'example_tests.py','--cli',offline[0],'--reference',reference,'--fixture',fixture,'--out',work/'offline-admission']
        if variant=='shared':arguments+=['--ffi',location]
        command(arguments,work,report,env)
        standalone=executable(offline_build,'engine-sim-render')
        standalone_arguments=arguments.copy();standalone_arguments[standalone_arguments.index('--cli')+1]=standalone
        standalone_arguments[standalone_arguments.index('--out')+1]=work/'standalone-admission'
        command(standalone_arguments,work,report,env)
        if variant=='shared':
            command([sys.executable,'-B',work/'ffi_smoke.py','--library',location,'--entry',fixture/'main.mr',
                     '--library-root',fixture/'library','--asset-root',fixture],work,report,env)
    if platform.system() == 'Linux':
        for name in ('runtime-c-consumer','runtime-cxx-consumer'):
            binary_path=executable(build,name)
            dynamic=command(['readelf','-d',binary_path],work,report)
            assert not any(x in dynamic.lower() for x in ('libsdl','libvulkan','libx11','libasound','libpulse'))
        if variant=='shared':
            library=location.resolve()
            exported=command(['nm','-D','--defined-only',library],work,report)
            assert {line.split()[-1] for line in exported.splitlines()}==PUBLIC
            dynamic=command(['readelf','-d',library],work,report)
            assert 'libengine-sim-runtime.so.1' in dynamic
            assert not any(x in dynamic.lower() for x in ('libsdl','libvulkan','libx11','libasound','libpulse'))
    elif platform.system() == 'Darwin':
        if variant=='shared':
            library=location.resolve()
            symbols=command(['nm','-gU',library],work,report)
            assert {line.split()[-1].removeprefix('_') for line in symbols.splitlines() if line.strip()}==PUBLIC
            command(['otool','-L',library],work,report)
    elif platform.system() == 'Windows':
        if variant=='shared':
            exported=command(['dumpbin','/exports',location],work,report)
            actual={line.split()[-1] for line in exported.splitlines() if line.split() and line.split()[-1].startswith('es_')}
            assert actual==PUBLIC
            command(['dumpbin','/dependents',location],work,report)
    return properties

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix',type=Path,required=True)
    parser.add_argument('--variant',choices=['static','shared'],required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--fixture',type=Path)
    parser.add_argument('--inside',action='store_true',help=argparse.SUPPRESS)
    parser.add_argument('--prepared',action='store_true',help='Consume an already verified minimal payload without a producer checkout')
    args=parser.parse_args();report=[];out=args.out.resolve()
    if args.inside:
        try:
            inside(args.prefix.resolve(),args.fixture.resolve(),out,args.variant,report)
            assert not Path('/home').exists() and not Path('/run').exists()
            result={'status':'pass','isolation':'Linux unshare-all; only system tools, minimal dev, copied consumer and moved SDK','commands':report}
        except Exception as error:
            result={'status':'fail','error':str(error),'commands':report}
        (out/'inside-result.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps({k:v for k,v in result.items() if k!='commands'}))
        return 0 if result['status']=='pass' else 1
    if args.prepared:
        prefix = args.prefix.resolve()
        if not out.is_dir() or not prefix.is_dir() or not prefix.is_relative_to(out):
            parser.error('Prepared SDK must be inside the separate consumer working directory')
        fixture = prefix/'share/engine-sim-runtime/example'
        try:
            if platform.system() == 'Linux':
                assert not Path('/home').exists() and not Path('/run').exists(), 'Linux prepared consumption requires its minimal namespace'
            inside(prefix,fixture,out,args.variant,report)
            result={'status':'pass','isolation':'Prepared consumer payload; producer absence and native tuple checked by transport driver',
                    'commands':report,'host':{'system':platform.system(),'machine':platform.machine(),'python':sys.version},
                    'installed_files':{p.relative_to(prefix).as_posix():file_sha256(p) for p in sorted(prefix.rglob('*')) if p.is_file()}}
        except Exception as error:
            result={'status':'fail','error':str(error),'commands':report}
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        print(json.dumps({k:v for k,v in result.items() if k not in ('commands','installed_files')}))
        return 0 if result['status']=='pass' else 1
    source=Path(__file__).resolve().parents[2]
    prefix=args.prefix.resolve()
    if out.is_relative_to(source) or out.exists() or not prefix.is_dir() or prefix.is_relative_to(out):
        parser.error('Choose a fresh external output directory and an existing separate installed prefix')
    out.mkdir(parents=True)
    moved=out/'SDK espaço Ω';shutil.move(str(prefix),str(moved))
    shutil.copytree(source/'examples/runtime/consumer',out/'consumer-source')
    shutil.copytree(source/'test/runtime/matrix',out/'matrix-source')
    shutil.copyfile(source/'test/runtime/ffi_matrix.py',out/'ffi_matrix.py')
    shutil.copyfile(source/'examples/runtime/ffi_smoke.py',out/'ffi_smoke.py')
    shutil.copyfile(__file__,out/'installed_consumer.py')
    extension='.exe' if platform.system()=='Windows' else ''
    if list(moved.rglob('engine-sim-render'+extension)):
        (out/'offline-source').mkdir()
        for name in ('CMakeLists.txt','render.cpp'):
            shutil.copyfile(source/'examples/runtime'/name,out/'offline-source'/name)
        shutil.copytree(source/'examples/runtime/consumer',out/'offline-source/consumer')
        shutil.copytree(source/'test/runtime/reference',out/'reference-source')
        shutil.copyfile(source/'test/runtime/example_tests.py',out/'example_tests.py')
    fixture=moved/'share/engine-sim-runtime/example'
    if args.fixture:
        fixture=out/'User fixture Ω';shutil.copytree(args.fixture.resolve(),fixture)
    if not (fixture/'main.mr').is_file():
        parser.error('Install the optional lawful example or pass --fixture to a caller-supplied fixture')
    try:
        if platform.system()=='Linux':
            if not shutil.which('bwrap'):raise RuntimeError('bwrap required for Linux source/build/device isolation')
            prefix_command=json.loads(os.environ.get('ENGINE_SIM_BWRAP_COMMAND','["bwrap"]'))
            if not isinstance(prefix_command,list) or not prefix_command or not all(isinstance(value,str) and value for value in prefix_command):
                raise RuntimeError('Invalid explicit namespace command prefix')
            argv=prefix_command+['--unshare-all','--die-with-parent','--new-session']
            for path in ('/usr','/lib','/lib64','/bin','/etc'):
                if Path(path).exists():argv+=['--ro-bind',path,path]
            argv+=['--proc','/proc','--dev','/dev','--tmpfs','/tmp','--bind',str(out),'/work','--chdir','/work','--clearenv',
                   '--setenv','PATH','/usr/bin:/bin','--setenv','LC_ALL','C.UTF-8','/usr/bin/python3','/work/installed_consumer.py',
                   '--inside','--prefix',str(Path('/work')/moved.name),'--variant',args.variant,'--out','/work','--fixture',str(Path('/work')/fixture.relative_to(out))]
            command(argv,out,report)
            result=json.loads((out/'inside-result.json').read_text());assert result['status']=='pass',result
        else:
            inside(moved,fixture,out,args.variant,report)
            result={'status':'pass','isolation':'moved prefix and copied consumers; producer and planning trees not hidden on this host. Separate clean native runner still required for source-hidden qualification.'}
        result['host']={'system':platform.system(),'machine':platform.machine(),'python':sys.version}
        result['original_prefix_absent']=not prefix.exists();assert result['original_prefix_absent']
        result['outer_commands']=report
        result['installed_files']={p.relative_to(moved).as_posix():file_sha256(p) for p in sorted(moved.rglob('*')) if p.is_file()}
    except Exception as error:
        result={'status':'fail','error':str(error),'outer_commands':report}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('commands','outer_commands','installed_files')},indent=2))
    return 0 if result['status']=='pass' else 1

if __name__=='__main__':sys.exit(main())
