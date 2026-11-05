from pathlib import Path
s=Path('/tmp/zyx-tree-focused-build.py').read_text().replace("Path('/tmp/zyx-tree-focused')","Path('/tmp/zyx-tree-libcpp')")
s=s.replace("'-std=c++20'","'-stdlib=libc++','-nostdinc++','-isystem','/tmp/zyx-libcxx-refund/sysroot/usr/lib/llvm-19/include/c++/v1','-std=c++20'")
s=s.replace("'-pthread','-lz'","'-L/tmp/zyx-libcxx-refund/sysroot/usr/lib/llvm-19/lib','-Wl,-rpath,/tmp/zyx-libcxx-refund/sysroot/usr/lib/llvm-19/lib','-pthread','-lz'")
exec(compile(s,'<libc++19 focused build>','exec'))
