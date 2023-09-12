# gst-webrtc-camera

## Description

* Gst-webrtc-camera project base on gstreamer,project function cover the offical's tutorial and more. i.e., hlssink,udpsink,appsink,splitmuxsink, and webrtc. It's privider offer webrtc camera and hls access and also record audio and video to file  triggered by timer or some signal.
* Built-in http server (libsoup) privider http and websocket access, gst-webrtc play as sendonly role of webrtc and html (RTCPeerConnection) play as recvonly role of webrtc. Also support Digest and Basic Authentication. Support get web login auth from sqlite3, Support webrtc and http access log record.

## Building

You'll need `meson`, the `gstreamer-plugins-bad,gstreamer-plugins-good` library, and the following librarys.

* gstreamer >= 1.23.0
* pkg-config
* libjson-glib-dev >= 1.66
* libsoup2.4-dev
* glib-2.0 >= 2.74.6
* sqlite3
* libasan6 (optional just for -fsanitize=address)

## Jetson Nano B01

* In Jetson nano, I tested my custom build ubuntu-20.10 (groovy) and it can run nvvidconv and nvarguscamerasrc together.

## RiotBoard(armv7l)

* In RiotBoard (imx6 armv7l), I tested it can run v4l2src and v4l2jpegdec together.

## pipeline workflow


  ![readme.svg](readme.svg)
---
* Playing diagram
 ![playing.png](playing.png)
---
* Webrtcbin diagram
 ![webrtcbin.png](webrtcbin.png)
 ![udpsrc.png](udpsrc.png)
---
* sequence diagram
 ![request](request.svg)

## webrtc complete diagram

 ![webrtc-complete-diagram.png](https://developer.mozilla.org/en-US/docs/Web/API/WebRTC_API/Connectivity/webrtc-complete-diagram.png)
