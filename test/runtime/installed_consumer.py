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
    result = subprocess.run(list(map(str, argv)), cwd=cwd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    report.append({'argv':list(map(str, argv)), 'cwd':str(cwd), 'exit':result.returncode, 'output':result.stdout})
    if result.returncode:
        raise RuntimeError(result.stdout)
    return result.stdout

def inside(prefix, fixture, work, variant, report):
    cmake = shutil.which('cmake')
    build = work/'consumer-build'
    command([cmake,'-S',work/'consumer-source','-B',build,'-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_PREFIX_PATH={prefix}',
             '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF','-DENGINE_SIM_CONSUMER_INSPECT=ON'],work,report)
    command([cmake,'--build',build,'--config','Release','--parallel','2','--verbose'],work,report)
    env = os.environ.copy()
    properties = (build/'imported-properties.txt').read_text()
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
    binary = build/'Release' if platform.system()=='Windows' else build
    extension = '.exe' if platform.system()=='Windows' else ''
    for name in ('runtime-c-consumer','runtime-cxx-consumer'):
        command([binary/(name+extension),fixture/'main.mr',fixture/'library',fixture],work,report,env)
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
        reference=reference_build/('Release' if platform.system()=='Windows' else '')/('runtime-direct-reference'+extension)
        arguments=[sys.executable,'-B',work/'example_tests.py','--cli',offline[0],'--reference',reference,'--fixture',fixture,'--out',work/'offline-admission']
        if variant=='shared':arguments+=['--ffi',location]
        command(arguments,work,report,env)
        standalone=offline_build/('Release' if platform.system()=='Windows' else '')/('engine-sim-render'+extension)
        standalone_arguments=arguments.copy();standalone_arguments[standalone_arguments.index('--cli')+1]=standalone
        standalone_arguments[standalone_arguments.index('--out')+1]=work/'standalone-admission'
        command(standalone_arguments,work,report,env)
        if variant=='shared':
            command([sys.executable,'-B',work/'ffi_smoke.py','--library',location,'--entry',fixture/'main.mr',
                     '--library-root',fixture/'library','--asset-root',fixture],work,report,env)
    if platform.system() == 'Linux':
        for name in ('runtime-c-consumer','runtime-cxx-consumer'):
            binary_path=binary/name
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
    source=Path(__file__).resolve().parents[2]
    prefix=args.prefix.resolve()
    if out.is_relative_to(source) or out.exists() or not prefix.is_dir() or prefix.is_relative_to(out):
        parser.error('Choose a fresh external output directory and an existing separate installed prefix')
    out.mkdir(parents=True)
    moved=out/'SDK espaço Ω';shutil.move(str(prefix),str(moved))
    shutil.copytree(source/'examples/runtime/consumer',out/'consumer-source')
    shutil.copyfile(__file__,out/'installed_consumer.py')
    extension='.exe' if platform.system()=='Windows' else ''
    if list(moved.rglob('engine-sim-render'+extension)):
        (out/'offline-source').mkdir()
        for name in ('CMakeLists.txt','render.cpp'):
            shutil.copyfile(source/'examples/runtime'/name,out/'offline-source'/name)
        shutil.copytree(source/'examples/runtime/consumer',out/'offline-source/consumer')
        shutil.copytree(source/'test/runtime/reference',out/'reference-source')
        shutil.copyfile(source/'test/runtime/example_tests.py',out/'example_tests.py')
        shutil.copyfile(source/'examples/runtime/ffi_smoke.py',out/'ffi_smoke.py')
    fixture=moved/'share/engine-sim-runtime/example'
    if args.fixture:
        fixture=out/'User fixture Ω';shutil.copytree(args.fixture.resolve(),fixture)
    if not (fixture/'main.mr').is_file():
        parser.error('Install the optional lawful example or pass --fixture to a caller-supplied fixture')
    try:
        if platform.system()=='Linux':
            if not shutil.which('bwrap'):raise RuntimeError('bwrap required for Linux source/build/device isolation')
            argv=['bwrap','--unshare-all','--die-with-parent','--new-session']
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
