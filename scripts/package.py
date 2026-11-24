"""Build the release archives into dist/; never publishes, trains or fetches a model."""
from pathlib import Path
import argparse, hashlib, json, os, platform, shutil, subprocess, sys, tarfile, re, zipfile

ROOT=Path(__file__).resolve().parents[1]
VERSION='0.1.0'

def run(*args,cwd=ROOT):
    subprocess.run(list(map(str,args)),cwd=cwd,check=True)

def copy_assets(target):
    target.mkdir(parents=True,exist_ok=True)
    shutil.copy2(ROOT/'resources/models/en-1/model.pwake',target/'model.pwake')
    shutil.copy2(ROOT/'resources/keywords/en.json',target/'keywords.json')

def windows_host():
    """WSL on a Windows host with Visual Studio, which scripts/build-windows.py drives."""
    return all(Path(p).is_file() for p in ['/mnt/c/Windows/System32/cmd.exe','/mnt/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'])

def windows_archive(path):
    """The Windows x64 library in the same layout as the Linux install tree: bin/, lib/, include/."""
    built=ROOT/'build/windows-x64'
    files=[(built/'kws_engine.dll','bin/kws_engine.dll'),(built/'kws_engine.lib','lib/kws_engine.lib'),
           (built/'phonlet_wake.h','include/phonlet_wake.h'),(ROOT/'LICENSE','LICENSE')]
    files+=[(ROOT/'THIRD_PARTY_NOTICES.md','THIRD_PARTY_NOTICES.md')] if (ROOT/'THIRD_PARTY_NOTICES.md').is_file() else []
    with zipfile.ZipFile(path,'w',zipfile.ZIP_DEFLATED) as archive:
        for source,name in files:
            archive.write(source,f'wake-native-{VERSION}/{name}')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-wasm',action='store_true',help='Use already rebuilt WASM outputs (recorded by hash).')
    parser.add_argument('--windows',choices=['auto','require','skip'],default='auto',
                        help='Windows x64 library via scripts/build-windows.py: auto builds it when WSL reaches a Windows host with Visual Studio.')
    args=parser.parse_args()
    run(sys.executable,ROOT/'scripts/check-repository.py')
    untracked=subprocess.check_output(['git','ls-files','--others','--exclude-standard'],cwd=ROOT,text=True).strip()
    if untracked: raise RuntimeError('Stage intended source files before packaging; untracked files would be omitted: '+untracked)
    out=ROOT/'dist';out.mkdir(exist_ok=True)
    work=ROOT/'build/packages'
    if work.exists(): shutil.rmtree(work)
    work.mkdir(parents=True)
    # A removed Python module must not survive in a subsequent wheel.
    if (ROOT/'sdk/python/build').exists(): shutil.rmtree(ROOT/'sdk/python/build')
    run(sys.executable,ROOT/'scripts/sync-js.py')
    for directory in [ROOT/'sdk/nodejs',ROOT/'sdk/web']:
        for path in sorted(directory.rglob('*')):
            if path.suffix in ('.mjs','.cjs') and 'node_modules' not in path.parts:
                run('node','--check',path)
    node=Path(shutil.which('node')).resolve()
    headers=node.parent.parent/'include/node'
    if not (headers/'node_api.h').is_file():
        headers=Path(os.environ.get('NODE_API_INCLUDE','/usr/include/node'))
    run('cmake','-S',ROOT/'engine','-B',ROOT/'build/native','-DCMAKE_BUILD_TYPE=Release','-DWAKE_BUILD_NODE=ON',f'-DNODE_API_INCLUDE={headers}')
    run('cmake','--build',ROOT/'build/native','-j2')
    if not args.skip_wasm:
        run('bash','engine/wasm/build.sh');run('bash','engine/wasm/build.sh','--simd')
    windows=args.windows=='require' or (args.windows=='auto' and windows_host())
    if windows: run(sys.executable,ROOT/'scripts/build-windows.py')
    else: print('Windows x64 library: skipped'+(' (no Windows host with Visual Studio)' if args.windows=='auto' else ''))
    # Target selection is deliberately qualified only for the actual build host.
    if (platform.system(),platform.machine())!=('Linux','x86_64'): raise RuntimeError('This candidate packager currently qualifies Linux x64 only')
    for relative in ['sdk/nodejs','sdk/web','sdk/python','packaging/model-node','packaging/model-python','packaging/native/linux-x64-gnu']:
        shutil.copy2(ROOT/'LICENSE',ROOT/relative/'LICENSE')
    model_node=work/'model-node';shutil.copytree(ROOT/'packaging/model-node',model_node,dirs_exist_ok=True);copy_assets(model_node)
    model_python=work/'model-python';shutil.copytree(ROOT/'packaging/model-python',model_python,dirs_exist_ok=True);copy_assets(model_python/'src/phonlet_wake_model_en')
    native=work/'native';shutil.copytree(ROOT/'packaging/native/linux-x64-gnu',native,dirs_exist_ok=True)
    shutil.copy2(ROOT/'build/native/wake_node.node',native/'wake_node.node')
    inputs={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((ROOT/'engine').rglob('*'))
            if p.is_file() and not any(x in ('build','__pycache__') for x in p.relative_to(ROOT/'engine').parts)}
    symbols=subprocess.check_output(['readelf','--version-info',str(native/'wake_node.node')],text=True)
    floors={prefix:max(set(re.findall(prefix+r'_([0-9.]+)',symbols)),key=lambda v:tuple(map(int,v.split('.')))) for prefix in ['GLIBC','GLIBCXX','CXXABI']}
    receipt={'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
             'source_worktree_dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True).strip()),
             'symbol_floors':floors,'version':VERSION,'target':'linux-x64-gnu','host':platform.platform(),'glibc':platform.libc_ver(),
             'node':subprocess.check_output(['node','--version'],text=True).strip(),
             'python':platform.python_version(),'compiler':subprocess.check_output(['c++','--version'],text=True).splitlines()[0],
             'engine_source_sha256':hashlib.sha256(json.dumps(inputs,sort_keys=True).encode()).hexdigest(),
             'node_api':8,'engine_abi':1,'qualification':'Linux x64 (build host glibc) and Windows x64; no manylinux or macOS claim'}
    (native/'build.json').write_text(json.dumps(receipt,indent=2)+'\n')
    for package in [ROOT/'sdk/nodejs',ROOT/'sdk/web',ROOT/'sdk/react',native,model_node]:
        run('npm','pack','--ignore-scripts','--pack-destination',out,cwd=package)
    run(sys.executable,'setup.py','sdist','--dist-dir',out,'bdist_wheel','--dist-dir',out,cwd=ROOT/'sdk/python')
    run(sys.executable,'setup.py','sdist','--dist-dir',out,'bdist_wheel','--dist-dir',out,cwd=model_python)
    install=work/'native-install'
    run('cmake','--install',ROOT/'build/native','--prefix',install)
    for notice in ['LICENSE','THIRD_PARTY_NOTICES.md']: shutil.copy2(ROOT/notice,install/notice)
    with tarfile.open(out/f'wake-native-{VERSION}-linux-x64-gnu.tar.gz','w:gz') as archive:
        archive.add(install,arcname=f'wake-native-{VERSION}')
    if windows: windows_archive(out/f'wake-native-{VERSION}-windows-x64.zip')
    run('node',ROOT/'demos/web/build.mjs',ROOT/'sdk/web',model_node)
    for name,source in [('web',ROOT/'demos/web/dist'),('nodejs',ROOT/'demos/nodejs'),('python',ROOT/'demos/python'),('native',ROOT/'demos/native')]:
        with tarfile.open(out/f'wake-demo-{name}-{VERSION}.tar.gz','w:gz') as archive:
            for p in sorted(source.rglob('*')):
                if p.is_file() and not any(part in ('node_modules','__pycache__','build') for part in p.relative_to(source).parts) and p.name!='test-fixture.json':
                    archive.add(p,arcname=str(Path(f'wake-demo-{name}')/p.relative_to(source)))
            if not (source/'LICENSE').is_file(): archive.add(ROOT/'LICENSE',arcname=f'wake-demo-{name}/LICENSE')
    tracked=subprocess.check_output(['git','ls-files','-z'],cwd=ROOT).decode().split('\0')
    with tarfile.open(out/f'wake-source-{VERSION}.tar.gz','w:gz') as archive:
        for name in tracked:
            if name and (ROOT/name).is_file():archive.add(ROOT/name,arcname=f'wake-{VERSION}/{name}')
    expected=[f'phonlet-ai-{name}-{VERSION}.tgz' for name in ['wake-node','wake-web','wake-react','wake-node-linux-x64-gnu','wake-model-en']]
    expected += [f'phonlet_wake-{VERSION}-py3-none-linux_x86_64.whl',f'phonlet_wake-{VERSION}.tar.gz',f'phonlet_wake_model_en-{VERSION}-py3-none-any.whl',f'phonlet_wake_model_en-{VERSION}.tar.gz',f'wake-native-{VERSION}-linux-x64-gnu.tar.gz',f'wake-source-{VERSION}.tar.gz']
    expected += [f'wake-demo-{name}-{VERSION}.tar.gz' for name in ['web','nodejs','python','native']]
    if windows:
        expected.append(f'wake-native-{VERSION}-windows-x64.zip')
        receipt['windows_x64']=json.loads((ROOT/'build/windows-x64/build.json').read_text())
    receipt['artifacts']={name:{'bytes':(out/name).stat().st_size,'sha256':hashlib.sha256((out/name).read_bytes()).hexdigest()} for name in sorted(expected)}
    (out/'release.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(f'Release artifacts: {out}')

if __name__=='__main__':main()
