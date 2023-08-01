
CC=gcc
CFLAGS=-g
EXE=v4l2-record

# https://makefiletutorial.com/#makefile-cookbook
# https://github.com/theicfire/makefiletutorial
#
CFLAGS=$$(pkg-config --cflags gstreamer-1.0 json-glib-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4)
LIBS=$$(pkg-config --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-app-1.0 gstreamer-base-1.0 libsoup-2.4 json-glib-1.0)
all:
	${CC} -Wall soup.c gst-app.c main.c -g -O0 -o ${EXE} ${CFLAGS} ${LIBS}


clean:
	rm ${EXE}
# libtool --mode=link gcc -Wall helloworld.c -o helloworld $(pkg-config --cflags --libs gstreamer-1.0)

