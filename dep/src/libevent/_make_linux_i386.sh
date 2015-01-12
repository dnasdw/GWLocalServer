#!/bin/sh

rootdir=`dirname $0`
cd $rootdir
rootdir=`pwd`
target=i386
prefix=$rootdir/$target
./configure --prefix="$prefix" --disable-shared
make
make install
rm -rf "$rootdir/../../include/$target/libevent"
mkdir "$rootdir/../../include/$target/libevent"
cp -rf "$prefix/include" "$rootdir/../../include/$target/libevent"
mkdir "$rootdir/../../lib/$target"
cp -rf "$prefix/lib/"libevent_* "$rootdir/../../lib/$target"
