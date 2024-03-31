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

## Install

* User config path at `~/.config/gwc`.

```sh
~$ dpkg -i install gwc-*.deb
~$ systemctl --user start gwc
~$ tail -f /tmp/gwc.log
```

## Install Development Environment (Optional)

```sh
~$ sudo apt-get install libgstreamer{1.0-dev,-plugins-{bad1.0-dev,base1.0-dev}} libsoup-3.0-dev libsqlite3-dev libjson-glib-dev -y
```

## Jetson Nano B01

* In Jetson nano, I tested my custom build ubuntu-20.10 (groovy) and it can run nvvidconv and nvarguscamerasrc together.

## RiotBoard(armv7l)

* In RiotBoard (imx6 armv7l), I tested it can run v4l2src and v4l2jpegdec together. You can download [Pre-built RiotBoard uSD Image](https://github.com/yjdwbj/imx6-riotboard) to testing this project.

## Picture Gallery

![mainview-control.png](images/mainview-control.png)
![mainview-detail.png](images/mainview-detail.png)
![hls-view.png](images/hls-view.png)
![webrtc-only-armv7l.png](images/webrtc-only-armv7l.png)
![zte-v520.png](images/zte-v520.png)


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
