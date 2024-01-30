
CC=gcc
CFLAGS :=-g -Wall -fno-omit-frame-pointer -fsanitize=address -DJETSON_NANO=$$(bash -c 'if  [ -f /etc/nv_tegra_release ]; then  echo 1; else echo 0; fi')
EXE=gst-webrtc-camera

# https://makefiletutorial.com/#makefile-cookbook
# https://github.com/theicfire/makefiletutorial
#
CFLAGS := $(CFLAGS) $$(pkg-config --cflags glib-2.0 gstreamer-1.0 json-glib-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-3.0 sqlite3)
LIBS=$$(pkg-config --libs glib-2.0 gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-app-1.0 gstreamer-base-1.0 libsoup-3.0 json-glib-1.0 sqlite3)
all:
	${CC} -Wall v4l2ctl.c sql.c soup.c gst-app.c main.c -g -O0 -o ${EXE} ${CFLAGS} ${LIBS}


clean:
# ifeq must be at the same indentation level in the makefile as the name of the target
ifneq (,$(wildcard $(EXE)))
	rm ${EXE}
endif


