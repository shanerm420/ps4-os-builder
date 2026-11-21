# Building with no root

The usual blocker on a locked-down machine is that `sudo` is refused and there is
no compiler. Neither is actually fatal. Two mechanisms do all the work, and both
are unprivileged.

## 1. apt with its own state directory

`apt` refuses to run as a normal user because it writes to `/var/lib/apt`,
`/var/cache/apt` and `/var/lib/dpkg`. Point those somewhere you own and it stops
caring:

```sh
S=$HOME/tc
mkdir -p "$S"/{etc/apt,var/lib/dpkg,var/cache/apt/archives/partial}
printf 'deb [trusted=yes] http://archive.ubuntu.com/ubuntu noble main universe\n' \
	> "$S/etc/apt/sources.list"
: > "$S/var/lib/dpkg/status"

APT="apt-get -o Dir=$S \
	-o Dir::Etc::sourcelist=$S/etc/apt/sources.list \
	-o Dir::Etc::sourceparts=/dev/null \
	-o Dir::State::status=$S/var/lib/dpkg/status \
	-o Dir::Cache=$S/var/cache/apt \
	-o Dir::State=$S/var/lib/apt \
	-o APT::Get::List-Cleanup=0"

$APT update -qq
$APT -y -qq --download-only --no-install-recommends install pahole flex bison bc
for d in "$S"/var/cache/apt/archives/*.deb; do dpkg-deb -x "$d" "$S"; done
```

Now `$S/usr/bin/pahole` exists and runs. Nothing was installed into `/`, nothing
needs root, and removing the directory removes everything.

**Use `--download-only install`, not `apt-get download`.** `download` needs package
lists it cannot write, and it does not resolve dependencies. `--download-only
install` resolves them and leaves the `.deb` files in `Dir::Cache`.

**Take host tools from the host's own release.** A binary from a newer release
links a newer glibc and will not start. That is why the script reads
`/etc/os-release` instead of hardcoding Debian trixie.

## 2. Upstream LLVM's release tarball for the compiler

A `clang` on its own is not enough: the kernel's archive and BTF steps call
`llvm-ar`, `llvm-objcopy`, `llvm-nm` and `llvm-strip`, and they are separate
programs. The official tarball carries all of them plus `ld.lld`:

```sh
curl -fL https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/\
clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04.tar.xz | tar -xJ -C "$HOME/tc/llvm" --strip-components=1
```

That build is portable (it was made on Ubuntu 18.04), with one exception: it links
`libtinfo.so.5`, which current distributions no longer ship. If `clang --version`
fails with `libtinfo.so.5: cannot open shared object file`, take that one library
out of an older release and put it next to the compiler - not on the global
`LD_LIBRARY_PATH`.

## The script

`bash scripts/bootstrap-toolchain.sh` does all of the above into
`~/.cache/ps4-toolchain` and prints what works:

```
  ok    clang          clang version 18.1.8
  ok    ld.lld         LLD 18.1.8
  ok    llvm-ar        LLVM
  ok    llvm-objcopy   llvm-objcopy
  ok    flex           flex 2.6.4
  ok    bison          bison 3.8.2
  ok    bc             bc 1.07.1
  ok    pahole         v1.25
  ok    zstd / cpio / depmod
```

Then `source ~/.cache/ps4-toolchain/env.sh`, and a kernel builds with:

```sh
make LLVM=1 olddefconfig
make LLVM=1 -j"$(nproc)"
```

## The traps, in the order they bite

| Symptom | Cause | Fix |
|---|---|---|
| `apt-get download` says the lists are empty | package lists live in the root state dir | private `Dir=` as above, and use `--download-only install` |
| `llvm-ar: command not found` | a clang-only install has no binutils | the LLVM release tarball, or the `llvm` package |
| host tools crash with stack smashing | a target sysroot is on `LD_LIBRARY_PATH`, so tools load its libc | never put a sysroot on `LD_LIBRARY_PATH`; use wrappers |
| `clang: libtinfo.so.5 not found` | upstream tarball predates libtinfo6 | copy just `libtinfo.so.5` into its own dir and wrap the compiler |
| `pahole` missing but `dwarves` installs | `dwarves` is transitional; the binary is in `pahole` | install `pahole` |
| sysroot linker errors about `/lib` and `/lib64` | Debian is merged-usr; those are symlinks on a real system | `ln -s usr/lib sysroot/lib`, same for `lib64` |
| bison/flex in a sysroot cannot find their data | they look in the compiled-in prefix | `BISON_PKGDATADIR=.../usr/share/bison`, `M4=.../bin/m4` |

## What this does not do

It does not give you a *target* sysroot. That is a separate, smaller job: the same
private-`Dir=` apt pointed at the release you are building for, `--download-only
install` of the `-dev` packages, `dpkg-deb -x` into a directory, then compile with
`--sysroot` - which is what `scripts/build-mesa.sh` in
[ps4-linux-desktop](https://github.com/shanerm420/ps4-linux-desktop) does to build
the PS4-patched Mesa against Debian 13's exact libraries.
