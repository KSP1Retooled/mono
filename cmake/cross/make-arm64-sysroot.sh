#!/bin/bash
# Unpacks an arm64 Ubuntu sysroot to cross-build against, and to run the result
# under qemu-user against.
#
#   cmake/cross/make-arm64-sysroot.sh <directory>
#
# Run it unprivileged. Its apt keeps all of its state under the sysroot and leaves
# the machine's own package lists and dpkg database alone.
#
# The release and the LLVM major match .github/actions/deps, so the cross build
# compiles against what the native arm64 CI job installs.
set -euo pipefail

if [ $# -ne 1 ]; then
	echo "usage: $0 <directory>" >&2
	exit 2
fi

release=noble
llvm_major=23
packages=(
	libc6-dev libgcc-13-dev libstdc++-13-dev
	zlib1g-dev libzstd-dev libgtest-dev
	"llvm-$llvm_major-dev"
)

mkdir -p "$1"
sysroot=$(cd "$1" && pwd)
apt=$sysroot/.apt
stamp=$sysroot/.complete

if [ -e "$stamp" ]; then
	echo "$sysroot is already complete; remove it to rebuild it"
	exit 0
fi

for tool in apt-get dpkg-deb curl; do
	command -v "$tool" > /dev/null || { echo "$0: $tool is not installed" >&2; exit 1; }
done

ubuntu_keyring=/usr/share/keyrings/ubuntu-archive-keyring.gpg
[ -e "$ubuntu_keyring" ] || { echo "$0: $ubuntu_keyring is missing" >&2; exit 1; }

mkdir -p "$apt"/{etc/preferences.d,etc/apt.conf.d,state/lists/partial,cache/archives/partial}
: > "$apt/state/status"

curl -fsSL -o "$apt/llvm.asc" https://apt.llvm.org/llvm-snapshot.gpg.key

cat > "$apt/etc/sources.list" <<EOF
deb [arch=arm64 signed-by=$ubuntu_keyring] http://ports.ubuntu.com/ubuntu-ports $release main universe
deb [arch=arm64 signed-by=$ubuntu_keyring] http://ports.ubuntu.com/ubuntu-ports $release-updates main universe
deb [arch=arm64 signed-by=$apt/llvm.asc] https://apt.llvm.org/$release/ llvm-toolchain-$release-$llvm_major main
EOF

# A configuration file rather than -o options, because apt reads the machine's
# /etc/apt/apt.conf.d before the command line.
cat > "$apt/apt.conf" <<EOF
Dir::Etc "$apt/etc";
Dir::Etc::sourcelist "$apt/etc/sources.list";
Dir::Etc::sourceparts "-";
Dir::Etc::parts "$apt/etc/apt.conf.d";
Dir::Etc::preferencesparts "$apt/etc/preferences.d";
Dir::State "$apt/state";
Dir::State::status "$apt/state/status";
Dir::Cache "$apt/cache";
APT::Architecture "arm64";
APT::Architectures { "arm64"; };
APT::Install-Recommends "false";
APT::Sandbox::User "";
Acquire::Languages "none";
Debug::NoLocking "true";
EOF
export APT_CONFIG=$apt/apt.conf

apt-get -q update

# The status file is empty, so this names the whole dependency closure down to
# libc.
apt-get -q -y --download-only install "${packages[@]}"

# A merged /usr, whether or not base-files is in the closure to bring it.
mkdir -p "$sysroot"/usr/{bin,sbin,lib}
for dir in bin sbin lib; do
	[ -e "$sysroot/$dir" ] || ln -s "usr/$dir" "$sysroot/$dir"
done

# tar rather than `dpkg-deb -x`, which replaces the /lib symlink with the
# directory a package that is not usr-merged ships.
for deb in "$apt"/cache/archives/*.deb; do
	dpkg-deb --fsys-tarfile "$deb" | tar -x -C "$sysroot" --keep-directory-symlink
done

# An absolute symlink resolves against the host's root rather than the
# sysroot's, and a -dev package writes its library links that way.
find "$sysroot" -path "$apt" -prune -o -type l -lname '/*' -print0 |
	while IFS= read -r -d '' link; do
		target=$(readlink "$link")
		ln -sfn "$(realpath -m -s --relative-to="$(dirname "$link")" "$sysroot$target")" "$link"
	done

test -e "$sysroot/lib/ld-linux-aarch64.so.1"
test -e "$sysroot/usr/lib/llvm-$llvm_major/lib/cmake/llvm/LLVMConfig.cmake"

rm -rf "$apt/cache"
touch "$stamp"
echo "arm64 sysroot ready at $sysroot"
