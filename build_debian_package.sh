#!/bin/bash

make clean
make

if [ -d deb_root ]; then
   rm -rf deb_root
fi

mkdir -pv deb_root
cp -a debian/gwc/ deb_root/

cp gwc webrtc-sendonly rtspsrc-webrtc deb_root/gwc/usr/sbin/
cp -a webroot create-server-cert.sh add_user.sh config.example deb_root/gwc/etc/gwc

hash_tag=$(git rev-parse --short HEAD)
dpkg-deb --root-owner-group -b deb_root/gwc ../gwc-git_${hash_tag}.deb

rm -rf deb_root

exit 0


