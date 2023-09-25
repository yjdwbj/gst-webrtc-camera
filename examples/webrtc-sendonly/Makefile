CC	:= gcc
LIBS	:= $(shell pkg-config --libs --cflags gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4 json-glib-1.0)
CFLAGS	:= -O0 -ggdb -Wall -fno-omit-frame-pointer -fsanitize=address

all: webrtc-sendonly rtspsrc-webrtc
webrtc-sendonly: webrtc-sendonly.c ../../v4l2ctl.c
	"$(CC)" $(CFLAGS) $^ $(LIBS) -o $@

rtspsrc-webrtc: rtspsrc-webrtc.c ../../v4l2ctl.c
	"$(CC)" $(CFLAGS) $^ $(LIBS) -o $@

clean:
	rm -rf webrtc-sendonly rtsp-webrtc