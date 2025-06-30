#!/bin/bash

# ARCH=$(dpkg --print-architecture)
# make clean
# make
VERSION=0.1.4
TAG=v${VERSION}

if [ -d deb_root ]; then
   rm -rf deb_root
fi

function create_debian_folder() {
   PKG_ARCH=$1
   DEB_PKG_ROOT=$2

   if [ -d ${DEB_PKG_ROOT} ]; then
      rm -rf ${DEB_PKG_ROOT}
   fi
   mkdir -pv ${DEB_PKG_ROOT}/DEBIAN
   mkdir -pv ${DEB_PKG_ROOT}/etc/gwc,
   mkdir -pv ${DEB_PKG_ROOT}/etc/default,systemd/user
   mkdir -pv ${DEB_PKG_ROOT}/etc/systemd/user
   mkdir -pv ${DEB_PKG_ROOT}/usr/sbin

   cat > ${DEB_PKG_ROOT}/DEBIAN/control<< EOF
Package: gst-webrtc-camera
Version: ${VERSION}
Architecture: ${PKG_ARCH}
Maintainer: Michael Liu <yjdwbj@gmail.com>
Installed-Size: 1421
Depends: libglib2.0-0 (>= 2.74),
        gstreamer1.0-nice,
        gstreamer1.0-x (>= 1.22),
        gstreamer1.0-opencv (>= 1.22),
        gstreamer1.0-plugins-bad (>= 1.22),
        gstreamer1.0-plugins-good (>= 1.22),
        gstreamer1.0-plugins-base (>= 1.22),
        gstreamer1.0-plugins-ugly (>= 1.22),
        libgstreamer-plugins-bad1.0-0 (>= 1.22),
        libgstreamer-plugins-base1.0-0 (>= 1.22),
        libgstreamer-opencv1.0-0 (>= 1.22),
        libgstreamer1.0-0 (>= 1.22),
        libsoup-3.0-0 (>= 3.2.2),
        libjson-glib-1.0-0 (>= 1.6),
        sqlite3 (>= 3.40),
        libudev1
Section: misc
Priority: optional
Homepage: https://github.com/yjdwbj/
Description: webrtc camera base on gstreamer,
 Gst-webrtc-camera project base on gstreamer,project function cover the offical's tutorial and more.
 ex: hlssink,udpsink,appsink,splitmuxsink, and webrtc. It's privider offer webrtc camera and hls access
 and also record audio and video to file triggered by timer or some signal.
EOF
   if [ ${PKG_ARCH} == "arm" ]; then
      sed -i "/^Architecture:/!b;cArchitecture: armhf" ${DEB_PKG_ROOT}/DEBIAN/control
   fi
   cat > ${DEB_PKG_ROOT}/DEBIAN/postinst <<EOF
#!/bin/bash

GWC_USER_PATH=/home/\${SUDO_USER}/.config/gwc/webroot
USER_SYSTEMD=/home/\${SUDO_USER}/.config/systemd/user/default.target.wants

mkdir -pv \${USER_SYSTEMD}
mkdir -pv \${GWC_USER_PATH}

chown \${SUDO_USER}:\${SUDO_USER} -R /home/\${SUDO_USER}/.config

if [ ! -d \${GWC_USER_PATH} ]; then
    echo "Not found old config files, then create new."
    mkdir -pv \${GWC_USER_PATH}
    cp -a /etc/gwc/config.example  \${GWC_USER_PATH}/config.json
    cp -a /etc/gwc/webroot/* \${GWC_USER_PATH}/webroot/
    cp -a /etc/gwc/*.sh \${GWC_USER_PATH}/
    cd \${GWC_USER_PATH}/
    ./create-server-cert.sh
    ./add_user.sh -u test -p test -r lcy-gsteramer-camera -d \${GWC_USER_PATH}/webrtc.db
    chown \${SUDO_USER}:\${SUDO_USER} -R \${GWC_USER_PATH}
else
   cp -a /etc/gwc/webroot/* \${GWC_USER_PATH}/
fi

su - \${SUDO_USER}  \
      -c "export XDG_RUNTIME_DIR=/run/user/\${SUDO_UID}  DBUS_SESSION_BUS_ADDRESS='unix:path=\${XDG_RUNTIME_DIR}/bus' ;\
      systemctl --user daemon-reload; systemctl --user start gwc; systemctl --user start webrtc-gwc"

exit 0
EOF
   chmod 755 ${DEB_PKG_ROOT}/DEBIAN/postinst

   cat > ${DEB_PKG_ROOT}/etc/default/rtsp-gwc<<EOF
# Default settings for rtsp-gwc.
# systemctl --user status rtsp-gwc

RTSP_URL=http://192.168.1.30
RTSP_USER=admin
RTSP_PWD=admin
RTSP_PORT=9002

EOF


 cat > ${DEB_PKG_ROOT}/etc/default/webrtc-gwc<<EOF
# Default settings for webrtc-sendonly.
# systemctl --user status webrtc-gwc

CAPS=video/x-raw,width=1280,height=720,framerate=10/1,format=YUY2
VIDEO_PATH=/mnt/nfs
UDPHOST=127.0.0.1
WEB_PORT=9001
AUTH_USER=admin
AUTH_PWD=admin
# 10 mintues
MAX_TIME=10


EOF

 cat > ${DEB_PKG_ROOT}/etc/systemd/user/webrtc-gwc.service <<EOF
[Unit]
Description=Gstreamer webrtc camera
Documentation=https://github.com/yjdwbj/gst-webrtc-camera
After=multi-user.target

[Service]
EnvironmentFile=/etc/default/webrtc-gwc
ExecStart=/usr/sbin/webrtc-sendonly -c \${CAPS} -u \${AUTH_USER} -p \${AUTH_PWD} --port=\${WEB_PORT} --udphost=\${UDPHOST} --max_time=\${MAX_TIME} --record_path=\${VIDEO_PATH}
Type=simple
StandardOutput=append:/tmp/webrtc-sendoly.log
StandardError=append:/tmp/webrtc-sendoly.log


[Install]
WantedBy=default.target
EOF
   chmod 755 ${DEB_PKG_ROOT}/etc/systemd/user/webrtc-gwc.service
   cat > ${DEB_PKG_ROOT}/etc/systemd/user/gwc.service <<EOF
[Unit]
Description=Gstreamer webrtc camera
Documentation=https://github.com/yjdwbj/gst-webrtc-camera
After=multi-user.target
StartLimitIntervalSec=20
StartLimitBurst=2


[Service]
ExecStart=/usr/sbin/gwc


Restart=on-failure
RestartSec=5s
Type=simple
StandardOutput=append:/var/log/gwc.log
StandardError=append:/var/log/gwc.log


[Install]
WantedBy=default.target
EOF
   chmod 755 ${DEB_PKG_ROOT}/etc/systemd/user/gwc.service
   cat > ${DEB_PKG_ROOT}/etc/systemd/user/rtsp-gwc.service <<EOF
[Unit]
Description=Rtsp redirect by gstreamer webrtc
Documentation=https://github.com/yjdwbj/gst-webrtc-camera
After=multi-user.target

[Service]
EnvironmentFile=/etc/default/rtsp-gwc
ExecStart=/usr/sbin/rtspsrc-webrtc -c \${RTSP_URL} -u \${RTSP_USER} -p \${RTSP_PWD} --port=\${RTSP_PORT}
Restart=no
Type=simple
StandardOutput=append:/tmp/rtsp-gwc.log
StandardError=append:/tmp/rtsp-gwc.log


[Install]
WantedBy=default.target
EOF
   chmod 755 ${DEB_PKG_ROOT}/etc/systemd/user/rtsp-gwc.service
}

function build_target() {
   TARGET_ARCH=$1
   case $TARGET_ARCH in
      arm)
         ARCH=${TARGET_ARCH}
         CROSS_COMPILE=arm-linux-gnueabi-
         CC=${CROSS_COMPILE}gcc
         SYSROOT=${HOME}/3TB-DISK/gitlab/docker-cross-compile-qt5/builds/riotboard/debian-4.0-stable/debian-armhf
         ;;
      arm64)
         ARCH=${TARGET_ARCH}
         CROSS_COMPILE=aarch64-linux-gnu-
         CC=${CROSS_COMPILE}gcc
         SYSROOT=${HOME}/3TB-DISK/gitlab/docker-cross-compile-qt5/builds/sun50i_a64-repo/debian-4.0-bookworm/debian-arm64
         ;;
      amd64)
          ARCH=${TARGET_ARCH}
          unset CROSS_COMPILE
          unset CC
          unset SYSROOT
          ;;
      *)
         echo "build native mode"
   esac

   local  DEB_PKG_ROOT=deb_root/${TARGET_ARCH}

   make  clean
   make ARCH=${ARCH} SYSROOT=${SYSROOT}

   create_debian_folder ${TARGET_ARCH} "${DEB_PKG_ROOT}"

   cp gwc webrtc-sendonly rtspsrc-webrtc ${DEB_PKG_ROOT}/usr/sbin/
   cp -a webroot create-server-cert.sh add_user.sh config.example ${DEB_PKG_ROOT}/etc/gwc

   hash_tag=$(git rev-parse --short HEAD)
   TARGET_FILE=../gwc-git-$(date +%Y-%m-%d)_${hash_tag}_${TAG}_${TARGET_ARCH}.deb
   [ -f ${TARGET_FILE} ] && rm -rf  ${TARGET_FILE}
   dpkg-deb --root-owner-group -b ${DEB_PKG_ROOT} ${TARGET_FILE}
   ./upload-github-release-asset.sh  tag="Latest" filename=${TARGET_FILE}
}


for arch in amd64 arm64 arm
do
   build_target $arch
done

echo "package done!!!"

exit 0


