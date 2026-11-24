"""Build the engine as a Windows x64 DLL with MSVC, driven from WSL.

  python scripts/build-windows.py                  # build into build/windows-x64/
  python scripts/build-windows.py --jobs 4   # or --work-dir 'D:\\phonlet-build\\wake'

The engine sources (engine/sources.txt) and headers are copied into a work directory on a Windows
drive, the language table is generated with the same generator as every other build, and cl.exe
compiles them with the MSVC options of engine/CMakeLists.txt (/O2 /fp:strict) plus a static C
runtime, so the DLL needs nothing beyond KERNEL32. /fp:strict never contracts a*b+c into a fused
multiply-add, and the x64 baseline (SSE2) has no FMA instruction; the build fails if an engine
object contains one. std::fmaf in the float kernels stays an explicit, correctly rounded call.

The DLL exports every C function of the engine: the public phonlet_wake_* API of
include/phonlet_wake.h and the kws_* functions the SDKs bind. Results land in build/windows-x64/:
kws_engine.dll, its import library kws_engine.lib, phonlet_wake.h and build.json (compiler, options,
exports and hashes). The same sources give the same DLL bytes: /Brepro, and no PDB, whose
multithreaded writing made the linked image differ from run to run.

Needs Visual Studio (2022 or later) with the C++ x64 tools on the Windows host and a Windows 10/11
SDK, either registered with Visual Studio or a NuGet layout given by --windows-sdk or
WAKE_WINDOWS_SDK (default C:\\_tools\\windows-sdk).
"""
from pathlib import Path, PureWindowsPath
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/windows-x64'
CMD = Path('/mnt/c/Windows/System32/cmd.exe')
VSWHERE = Path('/mnt/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe')
DEFAULT_SDK = r'C:\_tools\windows-sdk'
EXPORTED = re.compile(r'(phonlet_wake|kws)_\w+')
# Same include directories as the kws_core target in engine/CMakeLists.txt.
INCLUDES = ['engine/include', 'include', 'engine/include/kws/detail', 'engine/src/session', 'generated']
CFLAGS = [
    '/nologo', '/c', '/std:c++17', '/permissive-', '/Zc:__cplusplus', '/utf-8', '/EHsc', '/GR',
    '/MT', '/O2', '/fp:strict', '/DNDEBUG', '/W3', '/Gy', '/Gw', '/Brepro',
]
LDFLAGS = [
    '/nologo', '/DLL', '/MACHINE:X64', '/OPT:REF', '/OPT:ICF', '/INCREMENTAL:NO', '/Brepro',
]
FMA = re.compile(r'\bvf(n?)m(add|sub)\w*', re.I)


def windows_path(path):
    return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()


def wsl_path(path):
    return Path(subprocess.check_output(['wslpath', '-u', str(path)], text=True).strip())


def run_cmd(script, work):
    """Run a batch file with cmd.exe; its output goes to <script>.log and is shown on failure."""
    result = subprocess.run([str(CMD), '/d', '/c', windows_path(script)], cwd=work,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = result.stdout.decode('utf-8', errors='replace').replace('\r\n', '\n')
    log = script.with_suffix('.log')
    log.write_text(text)
    if result.returncode:
        sys.stdout.write(text)
        raise SystemExit(f'{script.name} failed with exit code {result.returncode}')
    warnings = len(re.findall(r': warning [A-Z]+\d+', text))
    if warnings:
        print(f'{script.stem}: {warnings} warnings, see {windows_path(log)}')
    return text


def visual_studio(explicit):
    if explicit:
        return PureWindowsPath(explicit)
    if not VSWHERE.is_file():
        raise SystemExit('vswhere.exe not found: install Visual Studio with the C++ x64 tools, or pass --vs')
    found = subprocess.check_output(
        [str(VSWHERE), '-latest', '-products', '*', '-requires',
         'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
    ).decode('utf-8', errors='replace').strip().splitlines()
    if not found:
        raise SystemExit('No Visual Studio with the C++ x64 tools found (vswhere)')
    return PureWindowsPath(found[0].strip())


def windows_sdk(explicit):
    """Environment lines for a Windows SDK, or [] when Visual Studio finds a registered one."""
    kits = Path('/mnt/c/Program Files (x86)/Windows Kits/10/Include')
    if not explicit and kits.is_dir() and any((v / 'ucrt/corecrt.h').is_file() for v in kits.iterdir()):
        return [], 'registered Windows Kits'
    sdk = PureWindowsPath(explicit or os.environ.get('WAKE_WINDOWS_SDK') or DEFAULT_SDK)
    include = wsl_path(sdk / 'headers/c/Include')
    versions = sorted((v.name for v in include.iterdir() if (v / 'ucrt/corecrt.h').is_file()),
                      key=lambda v: tuple(int(x) for x in v.split('.'))) if include.is_dir() else []
    if not versions or not (wsl_path(sdk / 'package/c/um/x64') / 'kernel32.lib').is_file():
        raise SystemExit(f'No Windows SDK: none registered with Visual Studio and none at {sdk} '
                         '(headers/c/Include/<version>, package/c/{um,ucrt}/x64)')
    headers = sdk / 'headers/c/Include' / versions[-1]
    lines = [
        f'set "INCLUDE={headers / "ucrt"};{headers / "um"};{headers / "shared"};%INCLUDE%"',
        f'set "LIB={sdk / "package/c/um/x64"};{sdk / "package/c/ucrt/x64"};%LIB%"',
    ]
    return lines, f'{sdk} {versions[-1]}'


def copy_sources(work):
    """A private copy of what the compiler reads, so a concurrent edit cannot mix into one build."""
    src = work / 'src'
    if src.exists():
        shutil.rmtree(src)
    for relative in ['engine/src', 'engine/include', 'include']:
        shutil.copytree(ROOT / relative, src / relative,
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    shutil.copy2(ROOT / 'engine/sources.txt', src / 'engine/sources.txt')
    (src / 'generated').mkdir()
    subprocess.run([sys.executable, ROOT / 'engine/tools/generate_languages.py', ROOT / 'engine/inventories',
                    src / 'generated/languages.generated.h'], check=True)
    sources = [line.strip() for line in (src / 'engine/sources.txt').read_text().splitlines() if line.strip()]
    return src, sources


def declared_api():
    text = (ROOT / 'include/phonlet_wake.h').read_text(encoding='utf-8')
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
    return sorted(set(re.findall(r'\b(phonlet_wake_\w+)\s*\(', text)))


def exported_functions(dumpbin_symbols):
    """Defined, external, C-linkage functions (no C++ decoration) named phonlet_wake_* or kws_*."""
    names = set()
    for line in dumpbin_symbols.splitlines():
        # 00A 00000000 SECT4  notype ()    External     | phonlet_wake_init
        match = re.match(r'^\s*[0-9A-F]+\s+[0-9A-F]+\s+(SECT\w+)\s+.*\(\)\s+External\s+\|\s+(\S+)$', line)
        if match and EXPORTED.fullmatch(match.group(2)):
            names.add(match.group(2))
    return sorted(names)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--work-dir', default=os.environ.get('WAKE_WINDOWS_WORK'),
                        help='Windows-side build directory (default: WAKE_WINDOWS_WORK, else %%TEMP%%\\phonlet-build\\wake)')
    parser.add_argument('--vs', help='Visual Studio installation directory (default: newest found by vswhere)')
    parser.add_argument('--windows-sdk', help=f'NuGet-layout Windows SDK (default: registered, else {DEFAULT_SDK})')
    parser.add_argument('--jobs', type=int, default=4, help='parallel compiler processes (default 4)')
    args = parser.parse_args()
    if not CMD.is_file():
        raise SystemExit('cmd.exe not reachable: run this from WSL on a Windows host')

    vs = visual_studio(args.vs)
    vcvars = vs / 'VC/Auxiliary/Build/vcvars64.bat'
    if not wsl_path(vcvars).is_file():
        raise SystemExit(f'{vcvars} not found')
    sdk_lines, sdk_name = windows_sdk(args.windows_sdk)
    if not args.work_dir:
        temp = subprocess.check_output([str(CMD), '/c', 'echo %TEMP%'], text=True, cwd='/mnt/c').strip()
        args.work_dir = temp + r'\phonlet-build\wake'
    work = wsl_path(args.work_dir)
    work.mkdir(parents=True, exist_ok=True)
    for name in ['obj', 'out']:
        if (work / name).exists():
            shutil.rmtree(work / name)
        (work / name).mkdir()
    src, sources = copy_sources(work)
    win_src = PureWindowsPath(windows_path(src))
    includes = ' '.join(f'/I"{win_src / d}"' for d in INCLUDES)
    files = ' '.join(f'"{win_src / "engine" / s}"' for s in sources)
    env = ['@echo off', 'setlocal', f'call "{vcvars}" >nul 2>&1', 'if errorlevel 1 exit /b 1', *sdk_lines]

    compile_script = work / 'compile.cmd'
    compile_script.write_text('\r\n'.join([
        *env,
        'cl 2>&1 | findstr /b Microsoft > obj\\compiler.txt',
        f'cl {" ".join(CFLAGS)} /MP{max(1, min(args.jobs, 4))} {includes} /Foobj\\ {files}',
        'if errorlevel 1 exit /b 1',
        'dumpbin /nologo /symbols obj\\*.obj > obj\\symbols.txt',
        'if errorlevel 1 exit /b 1',
        'dumpbin /nologo /disasm obj\\*.obj > obj\\code.txt',
        'if errorlevel 1 exit /b 1',
    ]) + '\r\n')
    print(f'compiling {len(sources)} sources with MSVC ({vs}), Windows SDK: {sdk_name}', flush=True)
    run_cmd(compile_script, work)

    # Check the compiled code of every engine object for fused multiply-add instructions.
    code = (work / 'obj/code.txt').read_text(errors='replace')
    fused = sorted(set(m.group(0).lower() for m in FMA.finditer(code)))
    if fused:
        raise SystemExit(f'engine objects contain fused multiply-add instructions {fused}; '
                         'the engine requires unfused float arithmetic')
    exports = exported_functions((work / 'obj/symbols.txt').read_text(errors='replace'))
    missing = [name for name in declared_api() if name not in exports]
    if missing:
        raise SystemExit(f'include/phonlet_wake.h declares functions no source defines: {missing}')
    (work / 'kws_engine.def').write_text('LIBRARY kws_engine\r\nEXPORTS\r\n' + ''.join(f'  {n}\r\n' for n in exports))

    link_script = work / 'link.cmd'
    link_script.write_text('\r\n'.join([
        *env,
        f'link {" ".join(LDFLAGS)} /DEF:kws_engine.def /OUT:out\\kws_engine.dll /IMPLIB:out\\kws_engine.lib '
        'obj\\*.obj',
        'if errorlevel 1 exit /b 1',
        'dumpbin /nologo /dependents out\\kws_engine.dll > out\\dependents.txt',
        'dumpbin /nologo /exports out\\kws_engine.dll > out\\exports.txt',
    ]) + '\r\n')
    run_cmd(link_script, work)

    dependents = sorted(set(re.findall(r'^\s+(\S+\.dll)\s*$', (work / 'out/dependents.txt').read_text(errors='replace'),
                                       flags=re.M | re.I)) - {'kws_engine.dll'})
    listed = re.findall(r'^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)', (work / 'out/exports.txt').read_text(errors='replace'),
                        flags=re.M)
    if sorted(listed) != exports:
        raise SystemExit(f'DLL exports differ from the definition file: {sorted(set(exports) ^ set(listed))}')
    if [d.upper() for d in dependents] != ['KERNEL32.DLL']:
        raise SystemExit(f'kws_engine.dll should import only KERNEL32.dll, imports {dependents}')

    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    for name in ['kws_engine.dll', 'kws_engine.lib']:
        shutil.copy2(work / 'out' / name, OUT / name)
    shutil.copy2(src / 'include/phonlet_wake.h', OUT / 'phonlet_wake.h')
    inputs = {str(p.relative_to(src)).replace('\\', '/'): sha256(p) for p in sorted(src.rglob('*')) if p.is_file()}
    git = lambda *a: subprocess.check_output(['git', *a], cwd=ROOT, text=True).strip()
    record = {
        'target': 'windows-x64',
        'compiler': (work / 'obj/compiler.txt').read_text(errors='replace').strip(),
        'visual_studio': str(vs),
        'windows_sdk': sdk_name,
        'cflags': CFLAGS, 'ldflags': LDFLAGS,
        'imports': dependents,
        'exports': exports,
        'source_commit': git('rev-parse', 'HEAD'),
        'source_worktree_dirty': bool(git('status', '--porcelain', '--', 'engine', 'include')),
        'engine_source_sha256': hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest(),
        'files': {name: {'bytes': (OUT / name).stat().st_size, 'sha256': sha256(OUT / name)}
                  for name in ['kws_engine.dll', 'kws_engine.lib', 'phonlet_wake.h']},
    }
    (OUT / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    dll = record['files']['kws_engine.dll']
    print(f'{OUT / "kws_engine.dll"}: {dll["bytes"]} bytes, {len(exports)} exports, imports {", ".join(dependents)}')


if __name__ == '__main__':
    main()
