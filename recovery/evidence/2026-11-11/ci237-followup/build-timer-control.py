from pathlib import Path
import concurrent.futures
import shlex
import shutil
import subprocess

root = Path('/workspace/zyx')
build = root / 'buildDir'
control = Path('/workspace/ci237-timer-control')
control.mkdir(exist_ok=True)
commands = subprocess.check_output(['ninja', '-t', 'commands', 'zyx_test_suite'], cwd=build, text=True).splitlines()
sources = ['src/utils/ChecksumUtils.cpp', 'src/storage/StorageIO.cpp', 'src/storage/relocatable/cache/BufferPool.cpp']
def compile_old(source):
    original = str(root / source)
    command = next(shlex.split(line) for line in commands if '-c ' + original in line)
    target = control / Path(source).name
    target.write_bytes(subprocess.check_output(['git', 'show', '7957d67bf:' + source], cwd=root))
    output = control / (Path(source).name + '.o')
    command[command.index('-c') + 1] = str(target)
    command[command.index('-o') + 1] = str(output)
    for flag in ['-MF', '-MT']:
        if flag in command: command[command.index(flag) + 1] = str(output) + ('.d' if flag == '-MF' else '')
    subprocess.run(command, cwd=build, check=True)
    return output
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    objects = list(pool.map(compile_old, sources))
archive = control / 'libzyx_core.a'
shutil.copyfile(build / 'libzyx_core.a', archive)
subprocess.run(['llvm-ar', 'rcs', str(archive), *map(str, objects)], check=True)
members = subprocess.check_output(['llvm-ar', 't', str(archive)], text=True).splitlines()
assert all(members.count(obj.name) == 1 for obj in objects)
line = next(line for line in commands if ' -o zyx_test_suite ' in line)
tokens = shlex.split(line)
first = next(i for i, token in enumerate(tokens) if 'clang++' in token)
last = next((i for i in range(first, len(tokens)) if tokens[i] == '&&'), len(tokens))
link = tokens[first:last]
link[link.index('-o') + 1] = '/workspace/ci237-before-static-timers'
link = [str(archive) if token == 'libzyx_core.a' else token for token in link]
subprocess.run(link, cwd=build, check=True)
print('Controlled executable: current objects with only three timer implementation TUs from 7957d67bf.')
