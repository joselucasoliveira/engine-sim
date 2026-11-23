"""CTest admission of actual installed, relocated SDK consumers."""
import argparse,json,os,shutil,subprocess,sys,tempfile
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--root',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--variant',choices=['static','shared'],required=True)
p.add_argument('--examples',action='store_true')
a=p.parse_args()
a.out.mkdir(parents=True,exist_ok=True)
run=Path(tempfile.mkdtemp(prefix=a.variant+'-',dir=a.out))
env=dict(os.environ,ES_OUT=str(a.root));commands=[]
def execute(argv, expected_failure=False):
 r=subprocess.run(list(map(str,argv)),cwd=a.source,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 commands.append({'argv':list(map(str,argv)),'cwd':str(a.source),'exit':r.returncode,'output':r.stdout})
 (run/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
 if expected_failure:
  assert r.returncode!=0,'Expected install preflight rejection'
 elif r.returncode:
  print(r.stdout);raise SystemExit(r.returncode)
 return r.stdout
name='runtime-'+a.variant
execute(['cmake','--preset',name,'-DENGINE_SIM_INSTALL_RUNTIME_EXAMPLE=ON']+(['-DENGINE_SIM_BUILD_RUNTIME_EXAMPLES=ON'] if a.examples else []))
execute(['cmake','--build','--preset',name,'--config','Release','--parallel','2'])
execute(['cmake','--install',a.root/name,'--config','Release','--prefix',run/'prefix','--component','RuntimeSDK'])
execute([sys.executable,'-B',a.source/'test/runtime/installed_consumer.py','--prefix',run/'prefix','--variant',a.variant,'--out',run/'consumers'])
result=json.loads((run/'consumers/result.json').read_text());assert result['status']=='pass'
print('Installed SDK admission passed:',run)

if a.variant=='static':
    # Configure a private source copy, then change one locked install input at a time.
    # Rejection must precede copying even the first SDK binary.
    projection=run/'input-check-source';projection.mkdir()
    for directory in ('cmake','include','modules/config','modules/json','schemas','src','scripting'):
        shutil.copytree(a.source/directory,projection/directory)
    for name in ('CMakeLists.txt','LICENSE','THIRD_PARTY_NOTICES.md'):
        shutil.copyfile(a.source/name,projection/name)
    (projection/'examples/runtime').mkdir(parents=True)
    shutil.copytree(a.source/'examples/runtime/fixture',projection/'examples/runtime/fixture')
    (projection/'dependencies/submodules').mkdir(parents=True)
    shutil.copyfile(a.source/'dependencies/CMakeLists.txt',projection/'dependencies/CMakeLists.txt')
    shutil.copyfile(a.source/'dependencies/submodules/CMakeLists.txt',projection/'dependencies/submodules/CMakeLists.txt')
    for name in ('simple-2d-constraint-solver','piranha'):
        shutil.copytree(a.source/'dependencies/submodules'/name,projection/'dependencies/submodules'/name,ignore=shutil.ignore_patterns('.git'))
        # Locked-source inspection remains bound to the exact producer gitlink.
        gitdir=subprocess.check_output(['git','-C',a.source/'dependencies/submodules'/name,'rev-parse','--absolute-git-dir'],text=True).strip()
        (projection/'dependencies/submodules'/name/'.git').write_text('gitdir: '+gitdir+'\n')
    execute(['cmake','-S',projection,'-B',run/'input-check-build','-DCMAKE_BUILD_TYPE=Release',
             '-DENGINE_SIM_BUILD_RUNTIME=ON','-DENGINE_SIM_BUILD_DESKTOP=OFF','-DENGINE_SIM_BUILD_CAPTURE=OFF',
             '-DBUILD_TESTING=OFF','-DENGINE_SIM_BUILD_HOST_TESTS=OFF','-DENGINE_SIM_INSTALL_RUNTIME_EXAMPLE=ON',
             f'-DENGINE_SIM_DEPS_FILE={a.root}/deps.cmake'])
    inputs = (projection/'examples/runtime/fixture/main.mr',
              projection/'schemas/engine-config-v1.schema.json')
    for fixture in inputs:
        original = fixture.read_bytes()
        rejected = run / ('refused-' + fixture.name)
        try:
            fixture.write_bytes(original + b'\n')
            message = execute(['cmake','--install',run/'input-check-build','--config','Release',
                               '--prefix',rejected,'--component','RuntimeSDK'], expected_failure=True)
            assert 'SDK input changed since configure' in message and fixture.name in message, message
            assert not rejected.exists() or not any(p.is_file() for p in rejected.rglob('*'))
        finally:
            fixture.write_bytes(original)
        print(f'Changed {fixture.name} rejected before any SDK file copy')
