
CC=gcc
CFLAGS=-g
EXE=gst-webrtc-camera

# https://makefiletutorial.com/#makefile-cookbook
# https://github.com/theicfire/makefiletutorial
#
CFLAGS=$$(pkg-config --cflags gstreamer-1.0 json-glib-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4)
LIBS=$$(pkg-config --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-app-1.0 gstreamer-base-1.0 libsoup-2.4 json-glib-1.0)
all:
	${CC} -Wall soup.c gst-app.c main.c -g -O0 -o ${EXE} ${CFLAGS} ${LIBS}


clean:
	rm ${EXE}

