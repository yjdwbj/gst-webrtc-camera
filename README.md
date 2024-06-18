# gst-webrtc-camera

## Description

* Gst-webrtc-camera project base on gstreamer,project function cover the offical's tutorial and more. i.e., hlssink,udpsink,appsink,splitmuxsink, and webrtc. It's privider offer webrtc camera and hls access and also record audio and video to file  triggered by timer or some signal.
* Built-in http server (libsoup) privider http and websocket access, gst-webrtc play as sendonly role of webrtc and html (RTCPeerConnection) play as recvonly role of webrtc. Also support Digest and Basic Authentication. Support get web login auth from sqlite3, Support webrtc and http access log record.

## Building

You'll need `meson`, the `gstreamer-plugins-bad,gstreamer-plugins-good` library, and the following librarys.

* gstreamer >= 1.22.0
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

## Install runtime Environment

```sh
~$ sudo apt-get install -y gstreamer1.0-x gstreamer1.0-opencv gstreamer1.0-plugins-bad \
        gstreamer1.0-plugins-good gstreamer1.0-plugins-base gstreamer1.0-plugins-ugly \
        libgstreamer-plugins-bad1.0-0 libgstreamer-plugins-base1.0-0 libgstreamer-opencv1.0-0 \
        libgstreamer1.0-0 libsoup-3.0-0 libjson-glib-1.0-0 sqlite3
```

## Install Development Environment (Optional)

```sh
~$ sudo apt-get install libgstreamer{1.0-dev,-plugins-{bad1.0-dev,base1.0-dev}} libsoup-3.0-dev libsqlite3-dev libjson-glib-dev -y
```

## Supported SBC

### Jetson Nano B01

* In Jetson nano, I tested my custom build ubuntu-20.10 (groovy) and it can run nvvidconv and nvarguscamerasrc together.

### RiotBoard(armv7l)

* In RiotBoard (imx6 armv7l), I tested it can run v4l2src and v4l2jpegdec together. You can download [Pre-built RiotBoard uSD Image](https://github.com/yjdwbj/imx6-riotboard) to testing this project.

### EAIDK-310 (rk3228h ARM64)

* In EAIDK-310 (rk3228h ARM64), I tested it can run v4l2src and v4l2jpegdec together. You can download [Pre-built EAIDK-310 uSD Image](https://github.com/yjdwbj/rockchip-eaidk-310) to testing this project.

### PINE A64+ (allwiner  sun50i-a64 ARM64)

* The Pine64 is a cost-optimized board sporting ARMv8 (64-bit ARM) capable cores. It was one of the first available boards with a 64-bit Allwinner chip, and one of the first affordable boards with an 64-bit ARM core in general. You can download [Pre-built PINE A64+ uSD Image](https://github.com/yjdwbj/sun50i-a64-pine64) to testing this project. It has enabled support for the Cedrus H.264 encoder.

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
