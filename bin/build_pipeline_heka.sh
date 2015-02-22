#!/bin/bash

# Exit on error:
set -o errexit

# Machine config:
# sudo yum install -y git hg golang cmake rpmdevtools GeoIP-devel rpmrebuild

BUILD_BRANCH=$1
if [ -z "$BUILD_BRANCH" ]; then
    BUILD_BRANCH=master
fi

BASE=$(pwd)
# To override the location of the Lua headers, use something like
#   export LUA_INCLUDE_PATH=/usr/include/lua5.1
if [ -z "$LUA_INCLUDE_PATH" ]; then
    # Default to the headers included with heka.
    LUA_INCLUDE_PATH=$BASE/build/heka/build/heka/include
fi

if [ ! -d build ]; then
    mkdir build
fi

cd build
if [ ! -d heka ]; then
    # Fetch a fresh heka clone
    git clone https://github.com/mozilla-services/heka
fi

cd heka

if [ ! -f "patches_applied" ]; then
    touch patches_applied

    echo "Patching for larger message size"
    patch message/message.go < $BASE/heka/patches/0001-Increase-message-size-limit-from-64KB-to-8MB.patch

    echo "Patching to build 'heka-export' cmd"
    patch CMakeLists.txt < $BASE/heka/patches/0002-Add-cmdline-tool-for-uploading-to-S3.patch

    echo "Patching to build 'heka-s3list' and 'heka-s3cat'"
    patch CMakeLists.txt < $BASE/heka/patches/0003-Add-more-cmds.patch

    # TODO: do this using cmake externals instead of shell-fu.
    echo "Installing source files for extra cmds"
    cp -R $BASE/heka/cmd/heka-export ./cmd/
    cp -R $BASE/heka/cmd/heka-s3list ./cmd/
    cp -R $BASE/heka/cmd/heka-s3cat ./cmd/

    echo 'Installing lua filters/modules/decoders'
    rsync -vr $BASE/heka/sandbox/ ./sandbox/lua/

    echo "Adding external plugin for s3splitfile output"
    echo "add_external_plugin(git https://github.com/mozilla-services/data-pipeline $BUILD_BRANCH heka/plugins/s3splitfile __ignore_root)" >> cmake/plugin_loader.cmake
fi

source build.sh

echo 'Installing lua-geoip libs'
cd $BASE/build
if [ ! -d lua-geoip ]; then
    # Fetch the lua geoip lib
    git clone https://github.com/agladysh/lua-geoip.git
fi
cd lua-geoip

# Use a known revision (current "master" as of 2015-02-12)
git checkout d9b36d7c70b7250a5c4e589d13c8b911df3c64fb

# from 'make.sh'
gcc -O2 -fPIC -I${LUA_INCLUDE_PATH} -c src/*.c -Isrc/ -Wall --pedantic -Werror --std=c99 -fms-extensions

SO_FLAGS="-shared -fPIC"
UNAME=$(uname)
case $UNAME in
Darwin)
    echo "Looks like OSX"
    SO_FLAGS="-bundle -undefined dynamic_lookup"
    ;;
*)
    echo "Looks like Linux"
    # Default flags apply.
    ;;
esac

HEKA_MODS=$BASE/build/heka/build/heka/modules
mkdir -p $HEKA_MODS/geoip
gcc $SO_FLAGS database.o city.o -o $HEKA_MODS/geoip/city.so
gcc $SO_FLAGS database.o country.o -o $HEKA_MODS/geoip/country.so
gcc $SO_FLAGS database.o lua-geoip.o -o $HEKA_MODS/geoip.so

echo 'Installing lua-gzip lib'
cd $BASE/build
if [ ! -d lua-gzip ]; then
    git clone https://github.com/vincasmiliunas/lua-gzip.git
fi
cd lua-gzip

# Use a known revision (current "master" as of 2015-02-12)
git checkout fe9853ea561d0957a18eb3c4970ca249c0325d84

gcc -O2 -fPIC -I${LUA_INCLUDE_PATH} $SO_FLAGS lua-gzip.c -lz -o $HEKA_MODS/gzip.so

echo 'Installing lua_hash lib'
cd $BASE
# Build a hash module with the zlib checksum functions
gcc -O2 -fPIC -I${LUA_INCLUDE_PATH} $SO_FLAGS heka/plugins/hash/lua_hash.c -lz -o $HEKA_MODS/hash.so

cd $BASE/build/heka/build

# Build RPM
make package
if hash rpmrebuild 2>/dev/null; then
    echo "Rebuilding RPM with date iteration and svc suffix"
    rpmrebuild -d . --release=0.$(date +%Y%m%d)svc -p -n heka-*-linux-amd64.rpm
fi