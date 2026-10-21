"""Relocate the host's linked dylib closure using Mach-O load commands and macOS tools."""
from pathlib import Path
import re
import shutil
import subprocess
import sys


def tool(*args):
    return subprocess.check_output([str(a) for a in args], text=True)


def system(path):
    return str(path).startswith(('/usr/lib/', '/System/Library/'))


def rpaths(binary, executable):
    paths = re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset', tool('otool', '-l', binary))
    return [Path(p.replace('@loader_path', str(binary.parent)).replace('@executable_path', str(executable.parent))) for p in paths]


def bundle(executable, origin=None):
    executable = Path(executable).resolve()
    frameworks = executable.parent.parent / 'Frameworks'
    origin = Path(origin).resolve() if origin else executable
    copied, names = {}, {}

    def visit(source, target, inherited):
        paths = rpaths(source, origin) + inherited
        identity = tool('otool', '-D', source).splitlines()[1:]
        dependencies = [line.strip().split(' (compatibility version', 1)[0]
                        for line in tool('otool', '-L', source).splitlines()[1:]]
        for dep in dependencies:
            if dep in identity or system(dep):
                continue
            expanded = dep.replace('@loader_path', str(source.parent)).replace('@executable_path', str(origin.parent))
            candidates = [p / expanded[7:] for p in paths] if expanded.startswith('@rpath/') else [Path(expanded)]
            resolved = next((p.resolve() for p in candidates if p.is_file()), None)
            if resolved is None:
                raise ValueError('Unresolved dependency: ' + dep)
            if system(resolved):
                replacement = str(resolved)
            else:
                if '.framework/' in str(resolved):
                    raise ValueError('Framework dependencies need a framework-aware packager: ' + dep)
                name = resolved.name
                if name in names and names[name] != resolved:
                    raise ValueError('Conflicting dylib basename: ' + name)
                names[name] = resolved
                replacement = '@executable_path/../Frameworks/' + name
                if resolved not in copied:
                    frameworks.mkdir(exist_ok=True)
                    destination = frameworks / name
                    shutil.copy2(resolved, destination)
                    destination.chmod(destination.stat().st_mode | 0o200)
                    copied[resolved] = destination
                    visit(resolved, destination, paths)
                    tool('install_name_tool', '-id', '@rpath/' + name, destination)
            tool('install_name_tool', '-change', dep, replacement, target)
        original_paths = re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset', tool('otool', '-l', target))
        for path in dict.fromkeys(original_paths):
            tool('install_name_tool', '-delete_rpath', path, target)

    visit(origin, executable, [])
    for library in copied.values():
        tool('codesign', '--force', '-s', '-', library)


if __name__ == '__main__':
    try:
        bundle(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
