#!/usr/bin/env python3
"""WebRTC transport for one emulator session: video, audio and keyboard.

Why this exists beside the VNC path rather than on top of it: VNC polls the X
framebuffer on the CPU and carries no sound, so the picture and the audio ended
up on two transports with no shared clock and no way to synchronise them. A
single webrtcbin carries both with RTP timestamps that do relate, encodes H.264
on the NVENC block of a GPU that is otherwise idle, and is built for real time
rather than for buffering.

Deliberately single-peer: a session has one player. A second connection replaces
the first rather than being multiplexed, which keeps the pipeline and the
signalling state trivial.
"""
import asyncio
import json
import os
import subprocess
import sys
import threading

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
gi.require_version("GstSdp", "1.0")
from gi.repository import Gst, GstWebRTC, GstSdp, GLib  # noqa: E402

import websockets  # noqa: E402

FPS      = int(os.environ.get("ZS1_WEBRTC_FPS", "50"))
BITRATE  = int(os.environ.get("ZS1_WEBRTC_BITRATE_KBPS", "12000"))
PORT     = int(os.environ.get("ZS1_WEBRTC_PORT", "6082"))
DISPLAY  = os.environ.get("DISPLAY", ":0")
SINK_MON = os.environ.get("ZS1_PULSE_MONITOR", "zs1.monitor")

# Audio encoder settings, per session like the video bitrate above.
#
# Opus at 96 kbit/s is ~5% of a 2.5 Mbit/s WAN session, so the bitrate is not
# where bandwidth goes; what matters on the wire is the packet rate. A 10 ms
# frame sends 100 packets a second and pays ~42% in RTP/SRTP/UDP/IP headers
# (136 kbit/s on the wire for 96 of payload); 20 ms halves both for 10 ms more
# delay, which is small against the ~110 ms the emulator itself buffers.
# The defaults keep today's behaviour; sessions.yaml picks 20 ms for the WAN.
DEFAULT_AUDIO_FRAME_MS = "10"
DEFAULT_AUDIO_TYPE = "restricted-lowdelay"
AUDIO_KBPS = int(os.environ.get("ZS1_WEBRTC_AUDIO_KBPS", "96"))
AUDIO_FRAME_MS = os.environ.get("ZS1_WEBRTC_AUDIO_FRAME_MS", DEFAULT_AUDIO_FRAME_MS)
AUDIO_TYPE = os.environ.get("ZS1_WEBRTC_AUDIO_TYPE", DEFAULT_AUDIO_TYPE)
AUDIO_FEC = os.environ.get("ZS1_WEBRTC_AUDIO_FEC", "0") == "1"
AUDIO_LOSS_PCT = int(os.environ.get("ZS1_WEBRTC_AUDIO_LOSS_PCT", "5"))

# The values opusenc's frame-size enum accepts (gst-inspect-1.0 opusenc, 1.24).
OPUS_FRAME_SIZES = ("2.5", "5", "10", "20", "40", "60")
# opusenc's audio-type enum: generic, voice, restricted-lowdelay.
OPUS_AUDIO_TYPES = ("generic", "voice", "restricted-lowdelay")
# Longest stall the audio queue absorbs before it starts dropping, in ns.
AUDIO_QUEUE_NS = 60_000_000
# How often a growing drop count is reported, in ms.
AUDIO_DROP_REPORT_MS = 10_000
BITS_PER_KBIT = 1000
MS_PER_S = 1000


def opus_encoder() -> str:
    """
    Build the opusenc element description from the ZS1_WEBRTC_AUDIO_* settings.

    In-band FEC exists only in Opus's LPC (SILK) layer, and restricted-lowdelay
    forces the CELT-only mode, so FEC asked for together with it would silently
    do nothing (libopus include/opus_defines.h, OPUS_SET_INBAND_FEC). The type is
    switched to generic in that case, and the switch is logged.

    Returns:
        The element text to place in the pipeline.
    """
    frame_ms = AUDIO_FRAME_MS if AUDIO_FRAME_MS in OPUS_FRAME_SIZES else DEFAULT_AUDIO_FRAME_MS
    audio_type = AUDIO_TYPE if AUDIO_TYPE in OPUS_AUDIO_TYPES else DEFAULT_AUDIO_TYPE
    if frame_ms != AUDIO_FRAME_MS or audio_type != AUDIO_TYPE:
        log(f"audio: ignoring unsupported frame '{AUDIO_FRAME_MS}' or type '{AUDIO_TYPE}'")
    if AUDIO_FEC and audio_type == "restricted-lowdelay":
        log("audio: in-band FEC needs the SILK layer; using audio-type=generic")
        audio_type = "generic"
    element = (f"opusenc bitrate={AUDIO_KBPS * BITS_PER_KBIT} frame-size={frame_ms} "
               f"audio-type={audio_type}")
    if AUDIO_FEC:
        element += f" inband-fec=true packet-loss-percentage={AUDIO_LOSS_PCT}"
    return element

# A relay, because the pipeline's own candidates go nowhere.
#
# The session runs on the k3d node's network, so every host candidate it gathers
# is a 172.19.0.x address on the Docker bridge — reachable from the machine that
# hosts the cluster and from nothing else, phone on the same wifi included. With
# a TURN server on an address the viewer can actually reach, webrtcbin also
# gathers a relay candidate there and the media has somewhere to land.
#
# The credential is derived, not stored. coturn runs with --use-auth-secret, so
# a valid username is an expiry timestamp and the password is its HMAC under a
# secret only the server and coturn hold. Nothing long-lived is ever handed to a
# browser: what it receives stops working after TURN_TTL seconds, so a credential
# captured from a page or a log is worth minutes rather than forever.
TURN_HOST   = os.environ.get("ZS1_WEBRTC_TURN_HOST", "")
TURN_SECRET = os.environ.get("ZS1_WEBRTC_TURN_SECRET", "")
TURN_TTL    = int(os.environ.get("ZS1_WEBRTC_TURN_TTL", "600"))


def turn_credentials():
    """A username/password pair coturn will accept until it expires."""
    import base64
    import hashlib
    import hmac
    import time
    user = f"{int(time.time()) + TURN_TTL}:zs1"
    pwd = base64.b64encode(
        hmac.new(TURN_SECRET.encode(), user.encode(), hashlib.sha1).digest()
    ).decode()
    return user, pwd

# nvcudah264enc, not nvh264enc.
#
# Both are present and both claim the GPU at registration time, but the older
# element drives the NVENC preset API that NVIDIA removed from the encoder SDK,
# so against a 610-series driver it registers, accepts the pipeline, and then
# fails to configure a session with "Selected preset not supported" — the stream
# never negotiates and the browser sits on "connecting". The newer element uses
# the current tune/rate-control API and works.
#
# openh264enc is the escape hatch for a machine with no usable NVENC; it costs
# CPU and latency, and is not the default for that reason.
ENCODER = os.environ.get("ZS1_WEBRTC_ENCODER", "nvcudah264enc")
if ENCODER == "nvcudah264enc":
    ENC = (f"nvcudah264enc name=venc bitrate={{BITRATE}} gop-size={{FPS}} "
           f"rate-control=cbr tune=ultra-low-latency zero-reorder-delay=true b-frames=0")
elif ENCODER == "openh264enc":
    ENC = "openh264enc name=venc bitrate={BITRATE}000 complexity=low"
else:
    ENC = ENCODER + " name=venc"

# 50 fps is not a throughput choice, it is the machine's own cadence: a PAL field
# is 20 ms, so anything above it transmits duplicate frames and anything below it
# drops real ones. gop-size follows at one keyframe a second.
#
# The video queues are one buffer deep and leaky on the capture side. A deeper
# queue would smooth a stall by adding delay, which is the opposite of what this
# is for: when the encoder falls behind, the right answer is to drop the frame
# that is already stale, not to show it late.
#
# Audio is different: a dropped video frame is invisible, a dropped 10 ms of
# sound is a click. Its queue used to be two buffers deep (20 ms), so any stall
# longer than that (CPU contention, a slow DTLS write, Python's garbage
# collector) discarded audio with nothing recording it. It now holds up to
# AUDIO_QUEUE_NS of audio, still leaky so a long stall cannot turn into lasting
# delay, and every drop is counted (see Session._on_audio_overrun).
def build_pipeline(audio_encoder: str) -> str:
    """
    Assemble the webrtcbin pipeline description.

    Args:
        audio_encoder: The opusenc element text, from opus_encoder().

    Returns:
        The text handed to Gst.parse_launch().
    """
    return f"""
webrtcbin name=sendrecv bundle-policy=max-bundle latency=0

ximagesrc display-name={DISPLAY} use-damage=0 show-pointer=false
  ! video/x-raw,framerate={FPS}/1
  ! queue max-size-buffers=1 leaky=downstream
  ! videoconvert
  ! {ENC.format(BITRATE=BITRATE, FPS=FPS)}
  ! h264parse config-interval=-1
  ! rtph264pay pt=96 config-interval=-1 aggregate-mode=zero-latency
  ! queue max-size-buffers=1 max-size-time=0 max-size-bytes=0
  ! sendrecv.

pulsesrc device={SINK_MON} provide-clock=false
  ! audio/x-raw,channels=2,rate=48000
  ! queue name=audioq leaky=downstream max-size-buffers=0 max-size-bytes=0 max-size-time={AUDIO_QUEUE_NS}
  ! audioconvert ! audioresample
  ! {audio_encoder}
  ! rtpopuspay pt=97
  ! queue max-size-buffers=0 max-size-bytes=0 max-size-time={AUDIO_QUEUE_NS}
  ! sendrecv.
"""


def log(*a):
    print("[webrtc]", *a, file=sys.stderr, flush=True)


class Session:
    def __init__(self, loop):
        self.loop = loop
        self.ws = None
        self.pipe = None
        self.webrtc = None
        self.audio_drops = 0
        self.audio_drops_reported = 0
        self.drop_timer = 0

    # -- signalling out: called from the GLib thread, delivered on the asyncio one
    def _send(self, obj):
        ws = self.ws
        if ws is None:
            return
        asyncio.run_coroutine_threadsafe(ws.send(json.dumps(obj)), self.loop)

    def start(self):
        self.stop()
        audio_encoder = opus_encoder()
        self.pipe = Gst.parse_launch(build_pipeline(audio_encoder))
        self.webrtc = self.pipe.get_by_name("sendrecv")
        # A leaky queue drops in silence; "overrun" is emitted each time it is
        # full when a buffer arrives, i.e. once per drop (checked against
        # GStreamer 1.24 with a consumer slowed to a third of real time).
        self.pipe.get_by_name("audioq").connect("overrun", self._on_audio_overrun)
        self.drop_timer = GLib.timeout_add(AUDIO_DROP_REPORT_MS, self._report_audio_drops)
        if TURN_HOST and TURN_SECRET:
            from urllib.parse import quote
            user, pwd = turn_credentials()
            self.webrtc.set_property(
                "turn-server", f"turn://{quote(user, safe='')}:{quote(pwd, safe='')}@{TURN_HOST}")
            log(f"turn: {TURN_HOST} (credential valid {TURN_TTL}s)")
        self.webrtc.connect("on-negotiation-needed", self._on_negotiation_needed)
        self.webrtc.connect("on-ice-candidate", self._on_ice_candidate)
        bus = self.pipe.get_bus()
        bus.add_signal_watch()
        bus.connect("message::error", self._on_error)
        bus.connect("message::warning",
                    lambda _b, m: log("warning:", m.parse_warning()[0].message))
        bus.connect("message::state-changed", self._on_state)
        rc = self.pipe.set_state(Gst.State.PLAYING)
        log(f"set_state(PLAYING) -> {rc.value_nick}; {FPS} fps, {BITRATE} kbps, {ENCODER}; "
            f"audio {audio_encoder}")

    def _on_audio_overrun(self, _queue):
        # Streaming thread; a plain int increment is enough under the GIL.
        self.audio_drops += 1

    def _report_audio_drops(self) -> bool:
        if self.audio_drops != self.audio_drops_reported:
            log(f"audio: {self.audio_drops - self.audio_drops_reported} buffers dropped "
                f"in the last {AUDIO_DROP_REPORT_MS // MS_PER_S} s ({self.audio_drops} total)")
            self.audio_drops_reported = self.audio_drops
        return True

    def _on_state(self, _bus, msg):
        if msg.src is self.pipe:
            old, new, _ = msg.parse_state_changed()
            log(f"pipeline {old.value_nick} -> {new.value_nick}")

    def stop(self):
        if self.drop_timer:
            GLib.source_remove(self.drop_timer)
            self.drop_timer = 0
        if self.pipe is not None:
            self.pipe.set_state(Gst.State.NULL)
            self.pipe = None
            self.webrtc = None

    def _on_error(self, _bus, msg):
        err, dbg = msg.parse_error()
        log("pipeline error:", err.message, "|", dbg)

    def _on_negotiation_needed(self, element):
        log("negotiation needed")

        # The reply is handled in a closure with exactly one user-data slot.
        # Passing two — the pattern in the older GStreamer examples — is silently
        # rejected by this binding: negotiation fired, create-offer ran, and the
        # callback simply never executed, so no offer was ever sent and the
        # browser sat waiting until it gave up with "signalling closed".
        #
        # Everything here is wrapped, because an exception raised inside a
        # GStreamer callback does not reach the interpreter's handler and would
        # vanish the same way.
        def on_offer(promise, _user_data=None):
            try:
                promise.wait()
                reply = promise.get_reply()
                offer = reply.get_value("offer") if reply is not None else None
                if offer is None or offer.sdp is None:
                    log("create-offer produced no SDP")
                    return
                # Read the text before handing the description over:
                # set-local-description takes ownership of the GstSDPMessage and
                # leaves offer.sdp None behind it.
                sdp_text = offer.sdp.as_text()
                log("offer created:", len(sdp_text), "bytes,",
                    sum(1 for l in sdp_text.splitlines() if l.startswith("m=")), "media")
                element.emit("set-local-description", offer, Gst.Promise.new())
                self._send({"sdp": {"type": "offer", "sdp": sdp_text}})
            except Exception as e:
                log("offer failed:", type(e).__name__, e)

        element.emit("create-offer", None, Gst.Promise.new_with_change_func(on_offer, None))

    def _on_ice_candidate(self, _element, mline, candidate):
        self._send({"ice": {"candidate": candidate, "sdpMLineIndex": mline}})

    # -- signalling in
    def on_answer(self, sdp_text):
        _res, sdpmsg = GstSdp.SDPMessage.new_from_text(sdp_text)
        answer = GstWebRTC.WebRTCSessionDescription.new(
            GstWebRTC.WebRTCSDPType.ANSWER, sdpmsg)
        self.webrtc.emit("set-remote-description", answer, Gst.Promise.new())

    def on_ice(self, ice):
        self.webrtc.emit("add-ice-candidate", ice["sdpMLineIndex"], ice["candidate"])


# Keyboard goes to the X server through xdotool rather than through the
# emulator: SDL is already reading the X input queue, so a synthetic key is
# indistinguishable from a real one and nothing in the emulator has to change.
def send_key(action, keysym):
    if not keysym or len(keysym) > 32 or not keysym.replace("_", "").isalnum():
        return
    subprocess.run(["xdotool", "key" + ("down" if action == "down" else "up"), keysym],
                   env={**os.environ, "DISPLAY": DISPLAY},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


async def handler(ws, session):
    if session.ws is not None:
        log("second viewer connected — replacing the first")
        try:
            await session.ws.close()
        except Exception:
            pass
    session.ws = ws
    if TURN_HOST and TURN_SECRET:
        # The viewer needs the same relay: on a network where neither side can
        # reach the other directly, both ends have to meet at the TURN server.
        # It gets its own short-lived pair, minted per connection.
        user, pwd = turn_credentials()
        await ws.send(json.dumps({"ice_servers": [
            {"urls": "turn:" + TURN_HOST, "username": user, "credential": pwd}]}))
    session.start()
    try:
        async for raw in ws:
            msg = json.loads(raw)
            if "sdp" in msg:
                session.on_answer(msg["sdp"]["sdp"])
            elif "ice" in msg:
                session.on_ice(msg["ice"])
            elif "key" in msg:
                send_key(msg["key"]["action"], msg["key"]["sym"])
    except websockets.ConnectionClosed:
        pass
    finally:
        if session.ws is ws:
            session.ws = None
            session.stop()
            log("viewer gone — pipeline stopped")


async def main():
    Gst.init(None)

    # GStreamer's signals need a GLib main loop; websockets needs an asyncio one.
    # They run side by side, and the only crossing is _send(), which hands the
    # message to asyncio from the GLib thread.
    glib_loop = GLib.MainLoop()
    threading.Thread(target=glib_loop.run, daemon=True).start()

    session = Session(asyncio.get_running_loop())
    async with websockets.serve(lambda ws: handler(ws, session), "0.0.0.0", PORT,
                                ping_interval=20, ping_timeout=20):
        log(f"signalling on :{PORT}")
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
