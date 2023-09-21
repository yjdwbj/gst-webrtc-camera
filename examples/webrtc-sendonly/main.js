'use strict';
var websocketConnection;
var webrtcPeerConnection;
var localpc;
var localStream;
var reportError;
var startWatch;

let reconnectTimerId;

let canvas;
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


let keyFrameCount = 0;
let interFrameCount = 0;
let keyFrameLastSize = 0;
let interFrameLastSize = 0;
let duplicateCount = 0;
let prevFrameType;
let prevFrameTimestamp;
let prevFrameSynchronizationSource;

let updateVideoStatusTimer;

let supportVideoSize = {};
let videoPixList;
let currentConstraint;
let bytesPrev;
let timestampPrev;

function videoAnalyzer(encodedFrame, controller) {
    const view = new DataView(encodedFrame.data);
    // We assume that the video is VP8.
    // TODO: Check the codec to see that it is.
    // The lowest value bit in the first byte is the keyframe indicator.
    // https://tools.ietf.org/html/rfc6386#section-9.1
    const keyframeBit = view.getUint8(0) & 0x01;
    if (keyframeBit === 0) {
        keyFrameCount++;
        keyFrameLastSize = encodedFrame.data.byteLength;
    } else {
        interFrameCount++;
        interFrameLastSize = encodedFrame.data.byteLength;
    }
    if (encodedFrame.type === prevFrameType &&
        encodedFrame.timestamp === prevFrameTimestamp &&
        encodedFrame.synchronizationSource === prevFrameSynchronizationSource) {
        duplicateCount++;
    }
    prevFrameType = encodedFrame.type;
    prevFrameTimestamp = encodedFrame.timestamp;
    prevFrameSynchronizationSource = encodedFrame.synchronizationSource;
    controller.enqueue(encodedFrame);
}

// var vConsole = new window.VConsole();
let iceServers = {};

async function onIceCandidate(event) {
    if (event.candidate == null || websocketConnection == null)
        return;

    // console.log("Sending ICE candidate out: " + JSON.stringify(event.candidate));
    await websocketConnection.send(JSON.stringify({ "type": "ice", "data": event.candidate }));
}

function reportError(err) {
    console.error(err);
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
            websocketConnection.send(JSON.stringify({
                "type": "v4l2",
                "data": { id: value.id, value: sval }
            }));
        }
    }
}

function addOnlineUserList(data) {
    document.getElementById('online-users').innerHTML = '';
    data.forEach((item) => {
        let div = '<li class="list-group-item d-flex justify-content-between align-items-start">' +
            '<div class="ms-2 me-auto">' +
            '<div class="fw-bold">' + item.name + '</div>' +
            'login :' + item.indate + '</div>' +
            '<span class="badge bg-primary rounded-pill">0</span></li>';
        document.getElementById('online-users').innerHTML += div;
    });
}

function createWebrtcRecv() {
    console.log("create webrtc receive peer.");
    webrtcPeerConnection = new RTCPeerConnection(Object.assign(iceServers, { encodedInsertableStreams: true }));
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
}

function inIframe() {
    try {
        return window.self !== window.top;
    } catch (e) {
        return true;
    }
}

window.onunload = function () {
    localStream.getTracks().forEach((track) => {
        track.stop();
    });

    clearInterval(updateVideoStatusTimer);
}

// buttons control
window.onload = function () {
    window.keyFrameCountDisplay = document.querySelector('#keyframe-count');
    window.keyFrameSizeDisplay = document.querySelector('#keyframe-size');
    window.interFrameCountDisplay = document.querySelector('#interframe-count');
    window.interFrameSizeDisplay = document.querySelector('#interframe-size');
    window.videoSizeDisplay = document.querySelector('#video-size');
    window.duplicateCountDisplay = document.querySelector('#duplicate-count');
    window.codecPreferences = document.getElementById('codecSelect');

    startWatch = document.getElementById('startWatch');
    // enableTalk = document.getElementById('enableTalk');
    screenShot = document.getElementById('screenShot');
    ctrlPanel = document.getElementById('ctrlPanel');

    canvas = document.getElementById('visualizer');
    canvasCtx = canvas.getContext('2d');


    // enableTalk.disabled = true;

    screenShot.disabled = true;
    ctrlPanel.disabled = true;

    if (inIframe())
        document.getElementById('title').remove();

    // if (document.querySelector('meta[name="uid"]').content == '999') {
    //     screenShot.remove();
    // }

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

            // $.getJSON('https://api.ipify.org?format=json', function (data) {
            //     let info = {
            //         client: {
            //             ip: data.ip,
            //             username: document.querySelector('meta[name="user"]').content,
            //             useragent: navigator.userAgent,
            //             path: document.location.pathname,
            //             origin: document.location.origin
            //         }
            //     };
            //     websocketConnection.send(JSON.stringify(info));
            // });
        })
            .catch((err) => {
                console.log("err is: " + err);
                reconnectTimerId = setTimeout(() => {
                    console.log("reconnect websockets");
                    playStream(null, null, null);
                }, 5000);
            });
    });

    function format(seconds) {
        var numhours = parseInt(Math.floor(((seconds % 31536000) % 86400) / 3600), 10);
        var numminutes = parseInt(Math.floor((((seconds % 31536000) % 86400) % 3600) / 60), 10);
        var numseconds = parseInt((((seconds % 31536000) % 86400) % 3600) % 60, 10);
        return ((numhours < 10) ? "0" + numhours : numhours) + ":" + ((numminutes < 10) ? "0" + numminutes : numminutes) + ":" + ((numseconds < 10) ? "0" + numseconds : numseconds);
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
    })
        .catch(reportError);
}

function showRemoteStats(results) {
    const peerDiv = document.getElementById('connected-to');
    const vbitrateDiv = document.getElementById('remote-vbitrate');
    const timestampDiv = document.getElementById('timestamp');
    const idDiv = document.getElementById('remote-id');
    const typeDiv = document.getElementById('remote-type');
    const sizeDiv = document.getElementById('video-dimension');
    if (videoCanvas) {
        sizeDiv.innerHTML = `<div class="fw-bold text-start">Video dimensions:</div>${videoCanvas.width}x${videoCanvas.height} px`;
    }

    // calculate video bitrate
    results.forEach(report => {
        const now = report.timestamp;
        timestampDiv.innerHTML = `<div class="fw-bold text-start" >Timestamp:</div>${now}`;
        idDiv.innerHTML = `<div class="fw-bold text-start" >Id:</div>${report.id}`;
        typeDiv.innerHTML = `<div class="fw-bold text-start" >Type:</div>${report.type}`;
        let bitrate;
        if (report.type === 'inbound-rtp' && report.mediaType === 'video') {
            const bytes = report.bytesReceived;
            if (timestampPrev) {
                bitrate = 8 * (bytes - bytesPrev) / (now - timestampPrev);
                bitrate = Math.floor(bitrate);
            }
            bytesPrev = bytes;
            timestampPrev = now;
        }
        if (bitrate) {
            bitrate += ' kbits/sec';
            vbitrateDiv.innerHTML = `<div class="fw-bold text-start" >Video Bitrate:</div>${bitrate}`;
        }

    });

    // figure out the peer's ip
    let activeCandidatePair = null;
    let remoteCandidate = null;

    // Search for the candidate pair, spec-way first.
    results.forEach(report => {
        if (report.type === 'transport') {
            activeCandidatePair = results.get(report.selectedCandidatePairId);
        }
    });
    // Fallback for Firefox.
    if (!activeCandidatePair) {
        results.forEach(report => {
            if (report.type === 'candidate-pair' && report.selected) {
                activeCandidatePair = report;
            }
        });
    }
    if (activeCandidatePair && activeCandidatePair.remoteCandidateId) {
        remoteCandidate = results.get(activeCandidatePair.remoteCandidateId);
    }
    if (remoteCandidate) {
        if (remoteCandidate.address && remoteCandidate.port) {
            peerDiv.innerHTML = `<div class="fw-bold text-start" data-bs-toggle="tooltip" data-bs-placement="top" title="${remoteCandidate.address}:${remoteCandidate.port}">Connected to:</div>${remoteCandidate.address}:${remoteCandidate.port}`;
        } else if (remoteCandidate.ip && remoteCandidate.port) {
            peerDiv.innerHTML = `<div class="fw-bold text-start" data-bs-toggle="tooltip" data-bs-placement="top" title="${remoteCandidate.ip}:${remoteCandidate.port}">Connected to:</div>${remoteCandidate.ip}:${remoteCandidate.port}`;
        } else if (remoteCandidate.ipAddress && remoteCandidate.portNumber) {
            // Fall back to old names.
            peerDiv.innerHTML = `<div class="fw-bold text-start" data-bs-toggle="tooltip" data-bs-placement="top" title="${remoteCandidate.ipAddress}:${remoteCandidate.port}">Connected to:</div>${remoteCandidate.ipAddress}:${remoteCandidate.portNumber}`;
        }
    }
}

async function onIncomingSDP(sdp) {
    if (sdp.type === "offer") {
        const showsdp = document.getElementById('showsdp');
        showsdp.value = '';
        showsdp.value = sdp.sdp;
        console.log("answser Sdp: " + showsdp.value);
        await webrtcPeerConnection.setRemoteDescription(sdp).catch(reportError);
        webrtcPeerConnection.createAnswer().then(onLocalDescription).catch(reportError);
        updateVideoStatusTimer = setInterval(() => {
            keyFrameCountDisplay.innerHTML = keyFrameCount;
            keyFrameSizeDisplay.innerHTML = keyFrameLastSize;
            interFrameCountDisplay.innerHTML = interFrameCount;
            interFrameSizeDisplay.innerHTML = interFrameLastSize;
            duplicateCountDisplay.innerHTML = duplicateCount;
            webrtcPeerConnection
                .getStats(null)
                .then(showRemoteStats, err => console.log(err));
        }, 1000);
    } else {
        // console.log("answser Sdp: " + sdp.sdp);
        await localpc.setRemoteDescription(sdp).catch(reportError);
    }
}

async function onIncomingICE(ice) {
    var candidate = new RTCIceCandidate(ice);
    await webrtcPeerConnection.addIceCandidate(candidate).catch(reportError);
}

function onAddRemoteStream(event) {
    // var el = document.createElement(event.track.kind)
    const el = document.querySelector('video');

    const frameStreams = event.receiver.createEncodedStreams();
    frameStreams.readable.pipeThrough(new TransformStream({
        transform: videoAnalyzer
    })).pipeTo(frameStreams.writable);
    el.addEventListener('resize', () => {
        videoSizeDisplay.innerText = `${el.videoWidth}x${el.videoHeight}`;
    });

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
    changeButtonState(screenShot, false);
    // changeButtonState(enableTalk, hasCamera ? false : true);
    changeButtonState(ctrlPanel, false);

    document.getElementById('loading').style['display'] = "none";
}

function changeButtonState(target, flag) {
    target.disabled = flag;
    target.classList.remove('btn-light');
    target.classList.add('btn-info');
}

function add_user_ctrls(ctrls) {
    userCtrls = ctrls;
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
                websocketConnection.send(JSON.stringify({
                    "type": "v4l2",
                    "data": { 'reset': true }
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

function onServerMessage(event) {
    var msg;

    try {
        msg = JSON.parse(event.data);
    } catch (e) {
        console.log("parse json error: " + e);
        return;
    }

    switch (msg.type) {
        case "sdp":
            onIncomingSDP(msg.data);
            break;
        case "ice":
            onIncomingICE(msg.data);
            break;
        case "record":
            recordCallBack(msg.data);
            break;
        case "users":
            addOnlineUserList(msg.data);
            break;
        case "iceServers": {
            iceServers = msg.iceServers;
            console.log(JSON.stringify(msg))
        }; default: break;
    }

    if (msg.ctrls) {
        add_user_ctrls(msg.ctrls);
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
                })
                    .catch(err => {
                        console.log("ws error: " + err);
                    });
            }, 5000);
        });
        websocketConnection.onopen = () => {
            resolve(websocketConnection);
        };
        websocketConnection.onerror = (err) => {
            reject(err);
        }
    });
}