# gst-webrtc-camera

## Description

* Gst-webrtc-camera project base on gstreamer,project function cover the offical's tutorial and more. ex: hlssink,udpsink,appsink,splitmuxsink, and webrtc. It's privider offer webrtc camera and hls access and also record audio and video to file  triggered by timer or some signal.
* Built-in http server (libsoup) privider http and websocket access, gst-webrtc play as sendonly role of webrtc and html (RTCPeerConnection) play as recvonly role of webrtc. Also support Digest and Basic authentication.

## pipeline workflow

* ![readme.svg](readme.svg)
* ![playing.png](playing.png)
* ![webrtcbin.png](webrtcbin.png)
* ![udpsrc.png](udpsrc.png)
