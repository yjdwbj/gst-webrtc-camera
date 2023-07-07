
CC=gcc
CFLAGS=-g
EXE=v4l2-record

# https://makefiletutorial.com/#makefile-cookbook
# https://github.com/theicfire/makefiletutorial
#
CFLAGS=$$(pkg-config --cflags gstreamer-1.0 json-glib-1.0)
LIBS=$$(pkg-config --libs gstreamer-1.0  json-glib-1.0)
all:
	${CC} -Wall  gst-app.c main.c -g -o ${EXE} ${CFLAGS} ${LIBS}


clean:
	rm ${EXE}
# libtool --mode=link gcc -Wall helloworld.c -o helloworld $(pkg-config --cflags --libs gstreamer-1.0)

