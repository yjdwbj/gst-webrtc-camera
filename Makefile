
CC=gcc
# -fsanitize=address is optional
CFLAGS :=-g -Wall -fno-omit-frame-pointer  -DJETSON_NANO=$$(bash -c 'if  [ -f /etc/nv_tegra_release ]; then  echo 1; else echo 0; fi')
EXE=gwc

# https://makefiletutorial.com/#makefile-cookbook
# https://github.com/theicfire/makefiletutorial
#
#
BLIBS	:= $(shell pkg-config --libs --cflags gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-3.0 json-glib-1.0 libudev)

CFLAGS := $(CFLAGS) $$(pkg-config --cflags glib-2.0 gstreamer-1.0 json-glib-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-3.0 sqlite3 libudev)
LIBS=$$(pkg-config --libs glib-2.0 gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-app-1.0 gstreamer-base-1.0 libsoup-3.0 json-glib-1.0 sqlite3 libudev)


all: webrtc-sendonly rtspsrc-webrtc gwc
webrtc-sendonly: webrtc-sendonly.c v4l2ctl.c common_priv.c media.c
	"$(CC)" $(CFLAGS) $^ $(BLIBS) -o $@

rtspsrc-webrtc: rtspsrc-webrtc.c v4l2ctl.c common_priv.c media.c
	"$(CC)" $(CFLAGS) $^ $(BLIBS) -o $@

gwc: v4l2ctl.c sql.c soup.c gst-app.c main.c common_priv.c media.c
	"${CC}" -Wall  -g -O0  ${CFLAGS} $^ ${LIBS} -o $@


clean:
# ifeq must be at the same indentation level in the makefile as the name of the target
ifneq (,$(wildcard $(EXE)))
	rm ${EXE} rtspsrc-webrtc  webrtc-sendonly
endif


