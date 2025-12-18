#!/bin/bash
#
# Produce a deb source package that can be uploaded to a Debian/Ubuntu builder.

if [[ $(git --no-optional-locks status -uno --porcelain) ]]; then
    echo "ERROR: git repository is not clean"
    echo "Try running:"
    echo "  git submodule foreach --recursive 'git reset --hard && git clean -fdx'"
    exit 1
fi

# Update submodules
git clean -xdf
git submodule update --init --recursive

# Clean up and include everything in the package
find subprojects/ -type d -name .git -exec rm -rf {} \;
find . -name .gitignore -exec rm -f {} \;

meson subprojects download

find subprojects/ -type d -name .git -exec rm -rf {} \;
find . -name .gitignore -exec rm -f {} \;

# Import submodules in git
git add .
git commit -s -a -m "[DROP THIS] Include submodules for packaging"

# Produce an orig tarball
mkdir -p deb
debversion=$(dpkg-parsechangelog -S Version | sed 's/^[0-9]\+://' | sed 's/-[^-]*$//')
git archive --prefix=qemu-10.1.0/ -o deb/qemu_$debversion.orig.tar.gz HEAD
