
# CC ?=gcc
# -fsanitize=address is optional
EXE=gwc

ifeq ($(ARCH),arm)
CFLAGS :=-g -Wall -fno-omit-frame-pointer
ARCH := arm
CROSS_COMPILE := arm-linux-gnueabi-
CC :=$(CROSS_COMPILE)gcc
SYSROOT :=${HOME}/3TB-DISK/gitlab/docker-cross-compile-qt5/builds/riotboard/debian-4.0-stable/debian-armhf
PKG_CONFIG_PATH :=${SYSROOT}/usr/lib/arm-linux-gnueabihf/pkgconfig
LDFLAGS :=--sysroot=${SYSROOT} -L${SYSROOT}/usr/lib/arm-linux-gnueabihf -Wl,-dynamic-linker,/lib/ld-linux-armhf.so.3 -Wl,-rpath-link ${SYSROOT}/usr/lib/arm-linux-gnueabihf
endif

ifeq ($(ARCH),arm64)
CFLAGS :=-g -Wall -fno-omit-frame-pointer   -DJETSON_NANO=$$(bash -c 'if  [ -f /etc/nv_tegra_release ]; then  echo 1; else echo 0; fi')
ARCH := arm64
CROSS_COMPILE := aarch64-linux-gnu-
CC :=$(CROSS_COMPILE)gcc
SYSROOT :=${HOME}/3TB-DISK/gitlab/docker-cross-compile-qt5/builds/sun50i_a64-repo/debian-4.0-bookworm/debian-arm64
PKG_CONFIG_PATH :=${SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig
LDFLAGS :=--sysroot=${SYSROOT} -L${SYSROOT}/usr/lib/aarch64-linux-gnu -Wl,-dynamic-linker,/lib/ld-linux-aarch64.so.1 -Wl,-rpath-link ${SYSROOT}/usr/lib/aarch64-linux-gnu
endif

ifeq ($(ARCH),amd64)
CFLAGS :=-g -Wall -fno-omit-frame-pointer

endif

# https://makefiletutorial.com/#makefile-cookbook
# https://github.com/theicfire/makefiletutorial
#
#

# ARMHF_CFLAGS := -I${SYSROOT}/usr/include \
# 				-I${SYSROOT}/usr/include/glib-2.0 \
# 				-I${SYSROOT}/usr/lib/arm-linux-gnueabihf/glib-2.0/include \
# 				-I${SYSROOT}/usr/include/gstreamer-1.0 \
# 				-I${SYSROOT}/usr/include/arm-linux-gnueabihf \
# 				-I${SYSROOT}/usr/include/json-glib-1.0 \
# 				-I${SYSROOT}/usr/include/libmount -I/usr/include/blkid \
# 				-I${SYSROOT}/usr/include/orc-0.4 -I/usr/include/libsoup-3.0 \
# 				-I${SYSROOT}/usr/include/sysprof-4 -pthread

CFLAGS := $(CFLAGS) $$(pkg-config --cflags glib-2.0 gstreamer-1.0 json-glib-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-3.0 sqlite3 libudev)
LIBS :=$(LDFLAGS) $$(pkg-config --libs glib-2.0 gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-app-1.0 gstreamer-base-1.0 libsoup-3.0 json-glib-1.0 sqlite3 libudev)
BLIBS	:=$(LDFLAGS) $(shell pkg-config --libs --cflags gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-3.0 json-glib-1.0 libudev)


all: webrtc-sendonly rtspsrc-webrtc gwc
webrtc-sendonly: webrtc-sendonly.c v4l2ctl.c common_priv.c media.c
	$(CC) $(CFLAGS) $^  $(BLIBS) -o $@

rtspsrc-webrtc: rtspsrc-webrtc.c v4l2ctl.c common_priv.c media.c
	$(CC) $(CFLAGS) $^  $(BLIBS) -o $@

gwc: v4l2ctl.c sql.c soup.c gst-app.c main.c common_priv.c media.c
	$(CC) -Wall  -g -O0  ${CFLAGS} $^  $(LIBS)  -o $@


clean:
# ifeq must be at the same indentation level in the makefile as the name of the target
ifneq (,$(wildcard $(EXE)))
	rm ${EXE} rtspsrc-webrtc  webrtc-sendonly
endif


