   'use strict';
    var websocketConnection;
    var webrtcPeerConnection;
    var localpc;
    var localdc;
    var localStream;
    var reportError;
    var startWatch;
    var startRecord;
    var enableTalk;
    var sendVoice;

    let reconnectTimerId;
    let isRecord = false;
    let isTalk = false;
    let isVoiceRecord = false;

    let canvas;
    let audioCtx;
    let canvasCtx;

    let hasCamera = false;
    let hasMicroPhone = false;

    // send record voice
    let lowWaterMark;
    let highWaterMark;
    const MAX_CHUNK_SIZE = 65536;
    let chunkSize;
    let mediaRecorder;
    let elapseTimer;

    // video snapshot
    let screenShot;
    let videoCanvas;
    let videoContent;
    let ctrlPanel;

    // v4l2 user ctrls

    let userCtrls;

    // var vConsole = new window.VConsole();
    const turn_config = {
      'iceServers': [{
        urls: 'turn:192.168.1.100:3478',
        username: "test",
        credential: "test123"
      }]
    };

    const stun_config = {
      'iceServers': [{
        urls: 'stun:stun.l.google.com:19302'
      }]
    };
    const supportedConstraints = navigator.mediaDevices.getSupportedConstraints();

    const audio_traints = {
      audio: {
        noiseSuppression: true,
        echoCancellation: true
      },
      video: false,

    };


    async function onIceCandidate(event) {
      if (event.candidate == null)
        return;

      // console.log("Sending ICE candidate out: " + JSON.stringify(event.candidate));
      await websocketConnection.send(JSON.stringify({ "type": "ice", "data": event.candidate }));
    }

    function reportError(err) {
      console.error(err);
    }

    function listCamerasAndMicrophone() {
      if (!navigator.mediaDevices || !navigator.mediaDevices.enumerateDevices) {
        console.log("enumerateDevices() not supported.");
      } else {
        // List cameras and microphones.
        navigator.mediaDevices
          .enumerateDevices()
          .then((devices) => {
            devices.forEach((device) => {
              console.log(`${device.kind}: ${device.label} \n,id = ${device.deviceId}`);
              if (device.kind === 'audioinput')
                hasMicroPhone = true;
              if (device.kind === 'videoinput')
                hasCamera = true;
            });
            console.log(`browser has camera: ${hasCamera}, has microphone: ${hasMicroPhone}`);
          })
          .catch((err) => {
            console.log(`${err.name}: ${err.message}`);
          });
      }
    }

    function toggleCanvas(show) {
      var vdom = document.getElementById('vis-dom');
      if (show === 'none') {
        vdom.style['height'] = '0%';
      } else {
        vdom.style['height'] = '10%';
      }
      vdom.style['display'] = show;
      canvas.style['display'] = show;
    }

    function stopLocalMedia() {
      if (localStream) {
        localStream.getTracks().forEach((track) => {
          track.stop();
        });
      }

      localpc.getTransceivers().forEach((transceiver) => {
        console.log("stop track.");
        transceiver.stop();
      });
      localpc.getSenders().forEach((sender) => {
        console.log("remove track.");
        localpc.removeTrack(sender);
      });
      toggleCanvas('none');
    }

    function createSender(stream) {
      toggleCanvas('block');
      console.log("stream: " + JSON.stringify(stream));
      localpc = new RTCPeerConnection(stun_config);
      if (hasCamera && hasMicroPhone) {
        localStream = stream;
        visualize(stream);
        stream.getTracks().forEach(function (track) {
          console.log("add track.");
          localpc.addTrack(track);
        });
      } else if (hasCamera) {
        console.log("just add video track");
        stream.getVideoTracks().forEach((track) => {
          localpc.addTrack(track);
        })
      } else if (hasMicroPhone) {
        localStream = stream;
        visualize(stream);
        stream.getAudioTracks().forEach((track) => {
          localpc.addTrack(track);
        })
      }

      localpc.createOffer().then(d => {
        localpc.setLocalDescription(d);
        console.log("Send offer to remote: " + d);
        websocketConnection.send(JSON.stringify({ type: "sdp", "data": d }));
      }).catch((err) => {
        console.log(" crete offer err: " + err);
      });
      localpc.oniceconnectionstatechange = e => {
        console.log("send stream state change: " + localpc.iceConnectionState);
        if (localpc.iceConnectionState == 'disconnected') {
          const pcShutdown = true;
          stopLocalMedia();
          isTalk = false;
          isVoiceRecord = false;
          enableTalk.innerHTML = isTalk ? "Video off" : "Video on";
          sendVoice.disabled = pcShutdown; //hasMicroPhone ? isTalk : true;
          sendVoice.innerHTML = isVoiceRecord ? "Send Voice" : "Start Voice";
        } else if (localpc.iceconnectionState === 'failed') {
          localpc.restartIce();
        }
      };
      localpc.onicecandidate = onIceCandidate;
      localpc.onsignalingstatechange = (ev) => {
        switch (localpc.signalingState) {
          case "stable":
            console.log("ICE negotiation complete");
            break;
        }
      };
    }

    function sendUserCtrl(evt) {
      const name = evt.target.id;
      for (let [key, value] of Object.entries(userCtrls)) {
        if (key == name) {
          let sval;
          if (evt.target.value == 'on') {
            sval = evt.target.checked ? 1 : 0;
          } else {
            sval = value.converted ? parseInt(evt.target.value, 10) - (0 - parseInt(value.max, 10) / 2) : parseInt(evt.target.value, 10);
            // console.log("target  is convertd: " + value.converted + " value change is: " + evt.target.value + " sval: " + sval);
          }
          localdc.send(JSON.stringify({
            "type": "v4l2",
            "ctrl": { id: value.id, value: sval }
          }));
        }
      }
    }

function addOnlineUserList(data) {
      document.getElementById('online-users').innerHTML = '';
      data.forEach((item) => {
        let div = '<li class="list-group-item d-flex justify-content-between align-items-start">' +
                  '<div class="ms-2 me-auto">' +
                  '<div class="fw-bold">'+ item.name + '</div>' +
                  'login :' + item.indate + '</div>' +
          '<span class="badge bg-primary rounded-pill">0</span></li>';
        document.getElementById('online-users').innerHTML += div;
      });
    }


    function createWebrtcRecv() {
      console.log("create webrtc receive peer.");
      webrtcPeerConnection = new RTCPeerConnection(stun_config);
      webrtcPeerConnection.ontrack = onAddRemoteStream;
      webrtcPeerConnection.onicecandidate = onIceCandidate;
      webrtcPeerConnection.addTransceiver('video', { 'direction': 'sendrecv' });
      webrtcPeerConnection.addTransceiver('audio', { 'direction': 'sendrecv' });
      webrtcPeerConnection.oniceconnectionstatechange = e => {
        console.log("receive stream state change: " + webrtcPeerConnection.iceConnectionState);
        if (webrtcPeerConnection.iceconnectionState === 'failed') {
          webrtcPeerConnection.restartIce();
        }
      };

      localdc = webrtcPeerConnection.createDataChannel("web page channel", { ordered: true });

      localdc.onmessage = (event) => {
        console.log(`received: ${event.data}`);
        let msg;
        try {
          msg = JSON.parse(event.data);
        } catch (e) {
          console.log("parse json error: " + e);
          return;
        }
        if (msg.notify) {
          alert(msg.notify);
        }
        if (msg.ctrls) {
          userCtrls = msg.ctrls;
          document.getElementById('ctrls').innerHTML = '';
          for (let [key, value] of Object.entries(userCtrls)) {
            // convert -10, 10 to 0 - 20 for input range.
            if (value.min < 0) {
              userCtrls[key].max = Math.abs(value.min) + parseInt(value.max, 10);
              userCtrls[key].value = Math.abs(value.min) + parseInt(value.value, 10);
              userCtrls[key].converted = true;
              userCtrls[key].min = 0;
            }
            let div;
            let min = userCtrls[key].min;
            let max = userCtrls[key].max;
            let step = userCtrls[key].step;
            let cval = userCtrls[key].value;
            if (value.type == 1) {
              div = '<li class="list-group-item"><label for="' + key + '" class="form-label">' + key + '</label>' +
                '<input type="range" class="form-range" value="' + cval + '"  min="' + min + ' " max="' + max + '" step="' + step + '" id="' + key + '"></li>';
            } else if (value.type == 2) {
              div = '<li class="list-group-item"><div class="custom-control custom-switch">' +
                '<input type="checkbox" class="custom-control-input" ' + (cval == 1 ? 'checked' : '') + ' id="' + key + '">' +
                '<label class="custom-control-label" for="' + key + '">' + key + '</label></div></li>';
            }
            if (div != null) {
              document.getElementById('ctrls').innerHTML += div;
              setTimeout(() => {
                console.log("run timeout");
                document.getElementById(key).addEventListener('change', sendUserCtrl);
              }, 200);
            }
          }
          // console.log(JSON.stringify(userCtrls));
          if (Object.keys(userCtrls).length) {
            document.getElementById('ctrls').innerHTML += '<button type="button" class="btn btn-primary" id="resetCtrls">Reset</button>'
            setTimeout(() => {
              document.getElementById('resetCtrls').addEventListener('click', (evt) => {
                localdc.send(JSON.stringify({
                  "type": "v4l2", "reset": true
                }));
                for (let [key, value] of Object.entries(userCtrls)) {
                  if (value.type == 1) {
                    if (value.converted) {
                      document.getElementById(key).value = parseInt(value.default, 10) - (0 - parseInt(value.max, 10) / 2);
                    } else {
                      document.getElementById(key).value = value.default;
                    }
                  } else if (value.type == 2) {
                    document.getElementById(key).checked = value.default > 0;
                  }
                }
              });
            }, 200);
          }
        }
      };

      localdc.onopen = () => {
        console.log("datachannel open");
        localdc.send("Hi, I'm browser!!!");
      };

      localdc.onclose = () => {
        console.log("datachannel close");
      };
    }

    function inIframe() {
      try {
        return window.self !== window.top;
      } catch (e) {
        return true;
      }
    }


    // buttons control
    window.onload = function () {
      listCamerasAndMicrophone();
      startWatch = document.getElementById('startWatch');
      startRecord = document.getElementById('startRecord');
      enableTalk = document.getElementById('enableTalk');
      sendVoice = document.getElementById('sendVoice');
      screenShot = document.getElementById('screenShot');
      ctrlPanel = document.getElementById('ctrlPanel');

      canvas = document.getElementById('visualizer');
      canvasCtx = canvas.getContext('2d');

      startRecord.disabled = true;
      enableTalk.disabled = true;
      sendVoice.disabled = true;
      screenShot.disabled = true;
      ctrlPanel.disabled = true;

      if (inIframe())
        document.getElementById('title').remove();


      document.querySelector('.visualizer').style['display'] = "none";

      startWatch.addEventListener('click', (event) => {
        event.preventDefault();
        console.log("start watch webrtc!!!");
        document.getElementById('loading').style['display'] = "block";
        startWatch.disabled = true;
        startWatch.classList.remove('btn-info');
        startWatch.classList.add('btn-light');
        // On firefox browser needed set media.peerconnection.use_document_iceservers = false
        // media.peerconnection.turn.disable = false
        playStream(null, null, null).then((ws) => {
          if (reconnectTimerId)
            clearTimeout(reconnectTimerId);

          $.getJSON('https://api.ipify.org?format=json', function (data) {
              let info = {
                  client: {
                      ip: data.ip,
                      username: document.querySelector('meta[name="user"]').content,
                      useragent: navigator.userAgent,
                      path: document.location.pathname,
                      origin: document.location.origin
                  }
              };
            websocketConnection.send(JSON.stringify(info));
          });

        }).catch((err) => {
          console.log("err is: " + err);
          reconnectTimerId = setTimeout(() => {
            console.log("reconnect websockets");
            playStream(null, null, null);
          }, 5000);
        });
      });

      startRecord.addEventListener('click', (event) => {
        event.preventDefault();
        console.log(startRecord.innerHTML + " on remote !!! " + isRecord);
        isRecord = !isRecord;
        startRecord.innerHTML = isRecord ? "Stop Record" : "Start Record";
        if (websocketConnection != undefined) {
          websocketConnection.send(JSON.stringify({ "type": "cmd", "cmd": "record", "arg": isRecord ? "start" : "stop" }));
        }

      });


      enableTalk.addEventListener('click', (event) => {
        console.log("enable talks now!!!");
        if (isTalk) {
          toggleCanvas('none');
          stopLocalMedia();
          websocketConnection.send(JSON.stringify({ "type": "cmd", "cmd": "talk", "arg": "stop" }));
          localStream = null;
        } else {
          // enableTalk.disabled = true;
          // To get navigator.mediaDevices, https should be enabled if not opened on localhost.
          console.log("enable talk has microphone ? " + hasMicroPhone);
          navigator.mediaDevices.getUserMedia({
            audio: hasMicroPhone ? {
              noiseSuppression: true,
              echoCancellation: true
            } : false,
            video: {
              width: { min: 800, ideal: 1280, max: 1920 },
              height: { min: 600, ideal: 720, max: 1080 },
            }
          }).then((stream) => {
            createSender(stream);
          }).catch((err) => {
            console.log("Capture media error: " + err);
          });
        }
        isTalk = !isTalk
        enableTalk.innerHTML = isTalk ? "Video off" : "Video on";
        sendVoice.disabled = hasMicroPhone ? isTalk : true;
        event.preventDefault();
      });

      function format(seconds) {
        var numhours = parseInt(Math.floor(((seconds % 31536000) % 86400) / 3600), 10);
        var numminutes = parseInt(Math.floor((((seconds % 31536000) % 86400) % 3600) / 60), 10);
        var numseconds = parseInt((((seconds % 31536000) % 86400) % 3600) % 60, 10);
        return ((numhours < 10) ? "0" + numhours : numhours)
          + ":" + ((numminutes < 10) ? "0" + numminutes : numminutes)
          + ":" + ((numseconds < 10) ? "0" + numseconds : numseconds);
      }

      function startRecordVoice(stream) {
        let chunks = [];
        console.log(" start record from stream");
        mediaRecorder = new MediaRecorder(stream);
        console.log("support audio/ogg;codecs=opus ?  " + MediaRecorder.isTypeSupported('audio/ogg;codecs=opus'));

        mediaRecorder.ondataavailable = (e) => {
          chunks.push(e.data);
        }

        mediaRecorder.onstop = (e) => {
          console.log("mediarecord on stop");
          const blob = new Blob(chunks, { 'type': 'audio/ogg; codecs=opus' });
          chunks = [];
          chunkSize = Math.min(webrtcPeerConnection.sctp.maxMessageSize, 16384);
          lowWaterMark = chunkSize;
          highWaterMark = blob.size;
          localdc.send(JSON.stringify({
            "type": "sendfile",
            "file": { 'type': 'audio/ogg; codecs=opus', 'size': blob.size, "name": "voice-" + performance.now() + ".ogg" }
          }));
          // localdc.binaryType = 'arraybuffer';
          localdc.bufferedAmountLowThreshold = lowWaterMark;

          var fileReader = new FileReader();
          let bufferedAmount = localdc.bufferedAmount;
          let numberSendCalls = 0;
          fileReader.onload = (e) => {
            console.log("file read blob load len : " + e.target.result.byteLength);
            while (bufferedAmount < highWaterMark) {
              numberSendCalls += 1;
              bufferedAmount += chunkSize;
              bufferedAmount = bufferedAmount > highWaterMark ? bufferedAmount + (bufferedAmount - highWaterMark) : bufferedAmount + chunkSize;
              const data = e.target.result.slice(localdc.bufferedAmount, bufferedAmount);
              localdc.send(data);
            }
          };
          fileReader.readAsArrayBuffer(blob);
        };
        mediaRecorder.start();
      }

      function makeElapseTimer(startDate) {
        var currDate = new Date();
        var diff = currDate - startDate;
        document.getElementById("elapse").innerHTML = format(diff / 1000);
      }

      function toggleElapseTimer(toStart) {
        if (toStart) {
          const nowDate = new Date();
          elapseTimer = setInterval(makeElapseTimer.bind(this, nowDate), 1000);
        } else {
          clearInterval(elapseTimer);
          document.getElementById("elapse").innerHTML = "00:00:00";
        }
        document.getElementById("elapse").style['display'] = toStart ? 'block' : 'none';
      }

      sendVoice.addEventListener('click', (event) => {
        event.preventDefault();
        if (isVoiceRecord) {
          console.log(" stop record voice.");
          mediaRecorder.stop();
          toggleElapseTimer(false);
          toggleCanvas('none');
          // send blob data;
        } else {
          console.log("start to record voice...");
          navigator.mediaDevices.getUserMedia(audio_traints).then((stream) => {
            toggleCanvas('block');
            visualize(stream);
            startRecordVoice(stream);
            sendVoice.disabled = true;
            toggleElapseTimer(true);
            setTimeout(() => {
              sendVoice.disabled = false;
            }, 2000);
          }).catch((err) => {
            console.log("Capture media error: " + err);
          });
        }

        isVoiceRecord = !isVoiceRecord;
        sendVoice.innerHTML = isVoiceRecord ? "Send Voice" : "Start Voice";
        enableTalk.disabled = isVoiceRecord;
      });

      screenShot.addEventListener('click', (evt) => {
        evt.preventDefault();
        const video = document.querySelector('video');
        console.log(`vidoe node name: ${video.nodeName}`);
        videoContent.fillRect(0, 0, videoCanvas.width, videoCanvas.height);
        // Grab the image from the video
        videoContent.drawImage(video, 0, 0, videoCanvas.width, videoCanvas.height);
        var m = new Date();
        var dateString = "ScreenShot-" +
          m.getUTCFullYear() +
          ("0" + (m.getUTCMonth() + 1)).slice(-2) +
          ("0" + m.getUTCDate()).slice(-2) + "_" +
          ("0" + m.getUTCHours()).slice(-2) +
          ("0" + m.getUTCMinutes()).slice(-2) +
          ("0" + m.getUTCSeconds()).slice(-2) + ".png";
        let fileName = dateString;
        fileName = prompt('Enter a name for image save?', dateString);
        if (fileName != null) {
          var link = document.createElement('a');
          link.download = fileName;
          link.href = videoCanvas.toDataURL('image/png', 1.0);
          link.click();
        } else {
          console.log("cancel save image!");
        }

      });

    }

    // recvonly webrtc
    async function onLocalDescription(desc) {
      console.log("Local description: " + JSON.stringify(desc));
      await webrtcPeerConnection.setLocalDescription(desc).then(function () {
        websocketConnection.send(JSON.stringify({ type: "sdp", "data": webrtcPeerConnection.localDescription }));
      }).catch(reportError);
    }


    async function onIncomingSDP(sdp) {
      console.log("Incoming SDP: " + JSON.stringify(sdp));
      if (sdp.type === "offer") {
        const showsdp = document.getElementById('showsdp');
        showsdp.value = '';
        showsdp.value = sdp.sdp;
        console.log("answser Sdp: " + showsdp.value);
        await webrtcPeerConnection.setRemoteDescription(sdp).catch(reportError);
        webrtcPeerConnection.createAnswer().then(onLocalDescription).catch(reportError);
      } else {
        // console.log("answser Sdp: " + sdp.sdp);
        await localpc.setRemoteDescription(sdp).catch(reportError);
      }
    }


    async function onIncomingICE(ice) {
      var candidate = new RTCIceCandidate(ice);
      if (startRecord.disabled == false && localpc) {
        console.log("answer Incoming ICE: " + JSON.stringify(ice));
        await localpc.addIceCandidate(candidate).catch(reportError);
      } else {
        await webrtcPeerConnection.addIceCandidate(candidate).catch(reportError);
      }
    }

    function recordCallBack(data) {
      alert("Recording thread is already running!!!");
      isRecord = false;
      startRecord.innerHTML = isRecord ? "Stop Record" : "Start Record";
    }

    function onAddRemoteStream(event) {
      // var el = document.createElement(event.track.kind)
      const el = document.querySelector('video');
      el.srcObject = event.streams[0]
      el.autoplay = true
      el.controls = true
      videoCanvas = document.getElementById('videoCanvas');
      videoContent = videoCanvas.getContext('2d');

      el.addEventListener('loadedmetadata', function () {
        videoCanvas.width = el.videoWidth;
        videoCanvas.height = el.videoHeight;
        console.log(`video size width ${videoCanvas.width}, height: ${videoCanvas.height}`);
      }, false);


      // enable other two buttons
      changeButtonState(startRecord, false);
      changeButtonState(screenShot, false);
      changeButtonState(enableTalk, hasCamera ? false : true);
      changeButtonState(sendVoice, hasMicroPhone ? false : true);
      changeButtonState(ctrlPanel, false);

      document.getElementById('loading').style['display'] = "none";
    }

    function changeButtonState(target, flag) {
      target.disabled = flag;
      target.classList.remove('btn-light');
      target.classList.add('btn-info');
    }

    function onServerMessage(event) {
      var msg;

      try {
        msg = JSON.parse(event.data);
      } catch (e) {
        console.log("parse json error: " + e);
        return;
      }

      switch (msg.type) {
        case "sdp": onIncomingSDP(msg.data); break;
        case "ice": onIncomingICE(msg.data); break;
        case "record": recordCallBack(msg.data); break;
        case "users": addOnlineUserList(msg.data); break;
        default: break;
      }
    }


    function playStream(hostname, port, path) {
      var l = window.location;
      var wsHost = (hostname != undefined) ? hostname : l.hostname;
      var wsPort = (port != undefined) ? port : l.port;
      var wsPath = (path != undefined) ? path : "ws";
      if (wsPort)
        wsPort = ":" + wsPort;
      var wsUrl = "wss://" + wsHost + wsPort + "/" + wsPath;

      createWebrtcRecv();
      return new Promise((resolve, reject) => {
        websocketConnection = new WebSocket(wsUrl);
        websocketConnection.addEventListener("message", onServerMessage);
        websocketConnection.addEventListener("close", () => {
          reconnectTimerId = setTimeout(() => {
            console.log("reconnect websockets");
            playStream(null, null, null).then(() => {
              if (reconnectTimerId)
                clearTimeout(reconnectTimerId);
            }).catch(err => {
              console.log("ws error: " + err);
            });
          }, 5000);
        });
        websocketConnection.onopen = () => {
          resolve(websocketConnection);
        }
        websocketConnection.onerror = (err) => {
          reject(err);
        }
      });
    }

    // visualizer handle
    function visualize(stream) {
      if (!audioCtx) {
        audioCtx = new AudioContext();
      }

      const source = audioCtx.createMediaStreamSource(stream);

      const analyser = audioCtx.createAnalyser();
      analyser.fftSize = 2048;
      const bufferLength = analyser.frequencyBinCount;
      const dataArray = new Uint8Array(bufferLength);

      source.connect(analyser);
      //analyser.connect(audioCtx.destination);

      draw()

      function draw() {
        const WIDTH = canvas.width
        const HEIGHT = canvas.height;
        requestAnimationFrame(draw);
        analyser.getByteTimeDomainData(dataArray);
        canvasCtx.fillStyle = 'rgb(200, 200, 200)';
        canvasCtx.fillRect(0, 0, WIDTH, HEIGHT);

        canvasCtx.lineWidth = 2;
        canvasCtx.strokeStyle = 'rgb(0, 0, 0)';

        canvasCtx.beginPath();

        let sliceWidth = WIDTH * 1.0 / bufferLength;
        let x = 0;

        for (let i = 0; i < bufferLength; i++) {
          let v = dataArray[i] / 128.0;
          let y = v * HEIGHT / 2;
          if (i === 0) {
            canvasCtx.moveTo(x, y);
          } else {
            canvasCtx.lineTo(x, y);
          }
          x += sliceWidth;
        }

        canvasCtx.lineTo(canvas.width, canvas.height / 2);
        canvasCtx.stroke();
      }
    }