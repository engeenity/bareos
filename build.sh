#!/bin/sh

# script for FreeNas 9.2.1 (bd35c86) jails

CFLAGS="-O3"
CXXFLAGS="-O3"

export CFLAGS CXXFLAGS

./configure \
--prefix=/mnt/vol1/jail/software/opt/bareos \
--with-plugindir=/mnt/vol1/jail/software/opt/bareos/lib/plugins \
--with-sbin-perm=0555 \
--enable-lockmgr \
--enable-includes \
--enable-ipv6 \
--enable-dynamic-cats-backends \
--disable-bat \
--with-sqlite3=/mnt/vol1/jail/software/usr/local/include \
--with-lzo=/usr/local \
--with-readline=/usr/local \
--disable-conio \
--enable-ndmp \
--enable-scsi-crypto
