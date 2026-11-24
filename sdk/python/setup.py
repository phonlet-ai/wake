"""Build a platform wheel with the owned engine; sdist carries all engine inputs."""
from pathlib import Path
import shutil
import subprocess
from setuptools import setup, Distribution
from setuptools.command.build_py import build_py
from setuptools.command.sdist import sdist
from wheel.bdist_wheel import bdist_wheel

HERE = Path(__file__).resolve().parent
ENGINE = HERE/'_engine' if (HERE/'_engine').is_dir() else HERE.parents[1]/'engine'

class NativeDistribution(Distribution):
    def has_ext_modules(self): return True

class Build(build_py):
    def run(self):
        super().run()
        native = Path(self.build_lib).resolve().parent/'cmake'
        subprocess.run(['cmake','-S',str(ENGINE),'-B',str(native),'-DCMAKE_BUILD_TYPE=Release','-DWAKE_BUILD_NODE=OFF'],check=True)
        subprocess.run(['cmake','--build',str(native),'--target','kws_engine','--config','Release','-j2'],check=True)
        target = Path(self.build_lib)/'phonlet_wake/_native'
        target.mkdir(parents=True,exist_ok=True)
        candidates=[*native.glob('libkws_engine.so'),*native.glob('libkws_engine.dylib'),*native.glob('Release/kws_engine.dll'),*native.glob('kws_engine.dll')]
        if len(candidates)!=1: raise RuntimeError(f'Expected one native library, found {candidates}')
        shutil.copy2(candidates[0].resolve(),target/candidates[0].name)

class Source(sdist):
    def make_release_tree(self, base_dir, files):
        super().make_release_tree(base_dir,files)
        # Include only engine build inputs, with no build outputs, datasets or other SDKs.
        target=Path(base_dir)/'_engine'
        for name in ['src','include','inventories','tools','cmake']:
            shutil.copytree(ENGINE/name,target/name,dirs_exist_ok=True,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
        for name in ['CMakeLists.txt','sources.txt']:
            shutil.copy2(ENGINE/name,target/name)
        # The public C header lives at the repository root; keep the same
        # relative layout so engine/CMakeLists.txt resolves it from the sdist.
        public=ENGINE.parent/'include'
        shutil.copytree(public,Path(base_dir)/'include',dirs_exist_ok=True)

class Wheel(bdist_wheel):
    def get_tag(self):
        _,_,platform=super().get_tag()
        return 'py3','none',platform

setup(distclass=NativeDistribution,cmdclass={'build_py':Build,'sdist':Source,'bdist_wheel':Wheel})
