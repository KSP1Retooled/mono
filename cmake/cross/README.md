# Running the arm64 tests on an amd64 machine

`mono/mini` and `mono/llvm` generate code for amd64 alone, so an arm64 build sets
`MONO_HAVE_CODEGEN` off (`cmake/MonoOptions.cmake`). It builds eglib, the utilities,
metadata, both collectors, `monodis`, `pedump` and the native support libraries, and
registers the unit tests that link only those. The runtime, the class libraries, BTLS
and every suite that runs managed code are left out.

CI builds the same thing natively in `ci.yml`'s `arm64` job. This directory is how to get
it on an amd64 Linux box or under WSL: compile with the host's clang against an arm64
sysroot, and run the result under qemu-user.

## Once per machine

```bash
sudo apt-get install clang lld ninja-build ccache qemu-user-static
cmake/cross/make-arm64-sysroot.sh ~/arm64-sysroot
```

The script needs no root. It downloads about 200MB and unpacks to 1.1GB.

`qemu-user-static` registers qemu with `binfmt_misc`, which is how a test that starts
another arm64 program reaches it. On a machine that also has `binfmt-support`, installing
it drops the handlers `binfmt-support` registered, such as the ones for `.exe` and `.jar`,
until the next boot. `sudo systemctl restart binfmt-support` puts them back.

## Build and test

```bash
git submodule update --init external/corefx
git submodule update --init --recursive external/bdwgc

cmake -S . -B build-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/cross/aarch64-linux-gnu.cmake" \
  -DMONO_CROSS_SYSROOT="$HOME/arm64-sysroot" \
  -DMONO_LLVM_PREFIX="$HOME/arm64-sysroot/usr/lib/llvm-23" \
  -DCMAKE_INSTALL_PREFIX="$PWD/tmp-arm64" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

cmake --build build-arm64 -j"$(nproc)"
ctest --test-dir build-arm64 -j"$(nproc)" --output-on-failure
```

`test-memfuncs/MemFuncs.MemmoveAtomic` takes three minutes under qemu. `-E MemmoveAtomic`
brings the rest to about ten seconds.

CTest sets `QEMU_LD_PREFIX` for every test. Set it by hand to run a binary directly:

```bash
QEMU_LD_PREFIX=~/arm64-sysroot build-arm64/mono/dis/monodis --help
```

Under `gdb`, let qemu serve the target and attach with `gdb-multiarch`:

```bash
QEMU_LD_PREFIX=~/arm64-sysroot qemu-aarch64-static -g 1234 <program> &
gdb-multiarch -ex "set sysroot $HOME/arm64-sysroot" -ex "target remote :1234" <program>
```

qemu-user translates arm64 to the host, and the host is amd64, whose memory model is
stronger. A missing barrier can therefore pass here and fail on hardware. Trust the CI
job over a local run for anything about memory ordering.

## Porting

Set `MONO_HAVE_CODEGEN` on for arm64 as the back end lands. `MONO_ENABLE_LIBRARIES` and
`MONO_ENABLE_MCS_BUILD` then stop being forced off, and the suites that need a runtime
register by themselves. `MONO_TARGET_ARCH` is what a source list branches on.
