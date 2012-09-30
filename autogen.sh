#!/bin/sh

set -e

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

pushd "$srcdir"
git submodule update --init --recursive
autoreconf -vfi
popd

if [ -z "$NOCONFIGURE" ]; then
    "$srcdir"/configure --enable-maintainer-mode "$@"
fi


