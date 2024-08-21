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

let runOnMoble = false;

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

window.mobileAndTabletCheck = function() {
    let check = false;
    (function(a){if(/(android|bb\d+|meego).+mobile|avantgo|bada\/|blackberry|blazer|compal|elaine|fennec|hiptop|iemobile|ip(hone|od)|iris|kindle|lge |maemo|midp|mmp|mobile.+firefox|netfront|opera m(ob|in)i|palm( os)?|phone|p(ixi|re)\/|plucker|pocket|psp|series(4|6)0|symbian|treo|up\.(browser|link)|vodafone|wap|windows ce|xda|xiino|android|ipad|playbook|silk/i.test(a)||/1207|6310|6590|3gso|4thp|50[1-6]i|770s|802s|a wa|abac|ac(er|oo|s\-)|ai(ko|rn)|al(av|ca|co)|amoi|an(ex|ny|yw)|aptu|ar(ch|go)|as(te|us)|attw|au(di|\-m|r |s )|avan|be(ck|ll|nq)|bi(lb|rd)|bl(ac|az)|br(e|v)w|bumb|bw\-(n|u)|c55\/|capi|ccwa|cdm\-|cell|chtm|cldc|cmd\-|co(mp|nd)|craw|da(it|ll|ng)|dbte|dc\-s|devi|dica|dmob|do(c|p)o|ds(12|\-d)|el(49|ai)|em(l2|ul)|er(ic|k0)|esl8|ez([4-7]0|os|wa|ze)|fetc|fly(\-|_)|g1 u|g560|gene|gf\-5|g\-mo|go(\.w|od)|gr(ad|un)|haie|hcit|hd\-(m|p|t)|hei\-|hi(pt|ta)|hp( i|ip)|hs\-c|ht(c(\-| |_|a|g|p|s|t)|tp)|hu(aw|tc)|i\-(20|go|ma)|i230|iac( |\-|\/)|ibro|idea|ig01|ikom|im1k|inno|ipaq|iris|ja(t|v)a|jbro|jemu|jigs|kddi|keji|kgt( |\/)|klon|kpt |kwc\-|kyo(c|k)|le(no|xi)|lg( g|\/(k|l|u)|50|54|\-[a-w])|libw|lynx|m1\-w|m3ga|m50\/|ma(te|ui|xo)|mc(01|21|ca)|m\-cr|me(rc|ri)|mi(o8|oa|ts)|mmef|mo(01|02|bi|de|do|t(\-| |o|v)|zz)|mt(50|p1|v )|mwbp|mywa|n10[0-2]|n20[2-3]|n30(0|2)|n50(0|2|5)|n7(0(0|1)|10)|ne((c|m)\-|on|tf|wf|wg|wt)|nok(6|i)|nzph|o2im|op(ti|wv)|oran|owg1|p800|pan(a|d|t)|pdxg|pg(13|\-([1-8]|c))|phil|pire|pl(ay|uc)|pn\-2|po(ck|rt|se)|prox|psio|pt\-g|qa\-a|qc(07|12|21|32|60|\-[2-7]|i\-)|qtek|r380|r600|raks|rim9|ro(ve|zo)|s55\/|sa(ge|ma|mm|ms|ny|va)|sc(01|h\-|oo|p\-)|sdk\/|se(c(\-|0|1)|47|mc|nd|ri)|sgh\-|shar|sie(\-|m)|sk\-0|sl(45|id)|sm(al|ar|b3|it|t5)|so(ft|ny)|sp(01|h\-|v\-|v )|sy(01|mb)|t2(18|50)|t6(00|10|18)|ta(gt|lk)|tcl\-|tdg\-|tel(i|m)|tim\-|t\-mo|to(pl|sh)|ts(70|m\-|m3|m5)|tx\-9|up(\.b|g1|si)|utst|v400|v750|veri|vi(rg|te)|vk(40|5[0-3]|\-v)|vm40|voda|vulc|vx(52|53|60|61|70|80|81|83|85|98)|w3c(\-| )|webc|whit|wi(g |nc|nw)|wmlb|wonu|x700|yas\-|your|zeto|zte\-/i.test(a.substr(0,4))) check = true;})(navigator.userAgent||navigator.vendor||window.opera);
    return check;
};

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

    runOnMoble = mobileAndTabletCheck();
    document.getElementById('title-pc').style['display'] = runOnMoble ? "none" : "block";
    document.getElementById('title-mobile').style['display'] = runOnMoble ? "block" : "none";


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