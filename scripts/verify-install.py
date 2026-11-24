"""Verify the release archives from independent consumer directories."""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,sys,tempfile,tarfile

root=Path(__file__).resolve().parents[1];dist=root/'dist'
manifest=json.loads((dist/'release.json').read_text())
for name,entry in manifest['artifacts'].items():
    data=(dist/name).read_bytes()
    assert len(data)==entry['bytes'] and hashlib.sha256(data).hexdigest()==entry['sha256'],name

def run(*args,cwd,env=None):
    return subprocess.run(list(map(str,args)),cwd=cwd,env=env,check=True,text=True,capture_output=True).stdout

with tempfile.TemporaryDirectory(prefix='wake-installed-') as directory:
    work=Path(directory);node=work/'node';node.mkdir()
    packages=[dist/f'phonlet-ai-{name}-0.1.0.tgz' for name in ['wake-node','wake-node-linux-x64-gnu','wake-model-en']]
    run('npm','install','--offline','--ignore-scripts','--no-audit','--no-fund',*packages,cwd=node)
    shutil.copy2(root/'tests/conformance.mjs',node/'conformance.mjs');(node/'fixtures').mkdir()
    shutil.copy2(root/'tests/fixtures/conformance.json',node/'fixtures/conformance.json')
    print(run('node','conformance.mjs',cwd=node).strip())
    run(sys.executable,'-m','venv',work/'venv',cwd=work)
    python=work/'venv/bin/python'
    run(python,'-m','pip','install','--disable-pip-version-check',dist/'phonlet_wake-0.1.0-py3-none-linux_x86_64.whl',dist/'phonlet_wake_model_en-0.1.0-py3-none-any.whl',cwd=work)
    shutil.copy2(root/'tests/installed.py',work/'installed.py');shutil.copy2(root/'tests/fixtures/conformance.json',work/'fixture.json')
    env={k:v for k,v in os.environ.items() if k not in ('PYTHONPATH','KWS_ENGINE_LIBRARY')};env['PATH']='/nonexistent'
    print(run(python,work/'installed.py',work/'fixture.json',cwd=work,env=env).strip())
    print(run(python,'-m','phonlet_wake.cli','--help',cwd=work,env=env).strip())
    # A packaged browser demo has no checkout or server API dependency.
    web=work/'web';web.mkdir()
    run('npm','install','--offline','--ignore-scripts','--no-audit','--no-fund',dist/'phonlet-ai-wake-web-0.1.0.tgz',dist/'phonlet-ai-wake-model-en-0.1.0.tgz',cwd=web)
    for name in ('build.mjs','index.html','app.mjs','styles.css'):shutil.copy2(root/'demos/web'/name,web/name)
    shutil.copytree(root/'assets/brand',web/'brand')
    print(run('node','build.mjs',web/'node_modules/@phonlet-ai/wake-web',web/'node_modules/@phonlet-ai/wake-model-en',web/'brand',cwd=web).strip())
    for path in sorted((web/'dist/vendor/wake').rglob('*.mjs')):
        run('node','--check',path,cwd=web)
    assert (web/'dist/vendor/wake/dist/kws_engine.wasm').is_file()
    assert (web/'dist/assets/model.pwake').is_file()
    for name in ('styles.css','brand/phonlet-ai-horizontal-dark.svg','brand/phonlet-ai-favicon-gradient.svg','brand/brand-tokens.css','LICENSE','README.txt'):
        assert (web/'dist'/name).is_file(), name
    # Rebuilding must not preserve stale generated files.
    (web/'dist/obsolete.js').write_text('stale')
    run('node','build.mjs',web/'node_modules/@phonlet-ai/wake-web',web/'node_modules/@phonlet-ai/wake-model-en',web/'brand',cwd=web)
    assert not (web/'dist/obsolete.js').exists()
    if '--types' in sys.argv:
        run('npm','install','--ignore-scripts','--no-audit','--no-fund','typescript@7.0.2',dist/'phonlet-ai-wake-web-0.1.0.tgz',cwd=node)
        for name in ('types.mts','types.cts'):shutil.copy2(root/'tests'/name,node/name)
        run(node/'node_modules/.bin/tsc','--noEmit','--strict','--module','NodeNext','--target','ES2022','--lib','ES2022,DOM','types.mts','types.cts',cwd=node)
        print('Strict ESM/CommonJS/browser TypeScript consumers passed.')
    print('Independent Node, Python and Web package installation passed.')
