# ZoniStation One as a cluster session: the emulator, an X server to draw into,
# and an audio sink, in one image.
#
# Nothing about the machine's content is baked in. BIOS and disc images arrive
# as read-only mounts at runtime, which is what keeps them out of any registry.
#
# -march=native is left as the Makefile has it. The k3d nodes are containers on
# the same host that builds this image, so the build CPU and the run CPU are the
# same i9. That stops being true the moment the image moves to another machine —
# there it must be rebuilt or given an explicit -march.

# ---------- build ----------
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential pkg-config cmake git ca-certificates xxd \
      libglew-dev libgl1-mesa-dev \
      libx11-dev libxext-dev libxkbcommon-dev libwayland-dev \
      libasound2-dev libpulse-dev libudev-dev \
      libvulkan-dev glslang-tools \
 && rm -rf /var/lib/apt/lists/*

# SDL3 is not packaged on Ubuntu 24.04, so it is built from source into
# /usr/local exactly as on the development machine; the Makefile finds it
# through pkg-config.
#
# 3.2.24 has a PulseAudio bug: after connecting it adopts fragsize instead of
# tlength for a playback stream (src/audio/pulseaudio/SDL_pulseaudio.c:735), so
# the emulator's 512-frame request comes back as buf=1024 in its "Audio:" log
# line. 3.4.0 fixes that line, but measured against this pod's null sink it
# reports buf=384 and PulseAudio's buffer latency for the stream goes from 6.6
# to 9.6 ms, so moving to it is not a clear latency win; it also needs
# libxcursor-dev and libxi-dev (and the other X11 -dev packages SDL lists) to
# configure. Kept at 3.2.24 until that is measured end to end.
ARG SDL_TAG=release-3.2.24
RUN git clone --depth 1 --branch ${SDL_TAG} https://github.com/libsdl-org/SDL.git /tmp/SDL \
 && cmake -S /tmp/SDL -B /tmp/SDL/build -DCMAKE_BUILD_TYPE=Release \
          -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
 && cmake --build /tmp/SDL/build -j"$(nproc)" \
 && cmake --install /tmp/SDL/build \
 && ldconfig \
 && rm -rf /tmp/SDL

WORKDIR /src
COPY . /src
RUN make -j"$(nproc)" && test -x ./ZoniStation_One

# ---------- runtime ----------
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
      xvfb x11-utils \
      libglew2.2 libgl1 libglx-mesa0 libegl1 libvulkan1 \
      libx11-6 libxext6 libxkbcommon0 libwayland-client0 \
      libasound2t64 libpulse0 libudev1 \
      pulseaudio pulseaudio-utils \
      x11vnc novnc websockify \
      ffmpeg xdotool \
      gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
      gstreamer1.0-plugins-bad gstreamer1.0-nice \
      python3-gi python3-websockets gir1.2-gst-plugins-bad-1.0 \
      fonts-dejavu-core \
 && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/lib/libSDL3.so* /usr/local/lib/
RUN ldconfig

COPY --from=builder /src/ZoniStation_One /opt/zs1/ZoniStation_One
COPY deploy/session/entrypoint.sh /opt/zs1/entrypoint.sh
# Dropped into noVNC's own web root so websockify serves it — the picture and
# the audio player end up on one page without a second HTTP server.
COPY deploy/session/play.html /usr/share/novnc/play.html
COPY deploy/session/webrtc.html /usr/share/novnc/webrtc.html
COPY deploy/session/webrtc_server.py /opt/zs1/webrtc_server.py
RUN chmod +x /opt/zs1/entrypoint.sh /opt/zs1/webrtc_server.py

# The NVIDIA container runtime exposes only compute+utility by default, which
# leaves the container with no libGLX/libEGL and no Vulkan ICD. Both renderers
# need graphics; display is what lets the driver present to the X server.
ENV NVIDIA_DRIVER_CAPABILITIES=graphics,display,video,compute,utility \
    NVIDIA_VISIBLE_DEVICES=all \
    DISPLAY=:0 \
    ZS1_UI=gameplay

# uid 1000 matches the host user that owns the bind-mounted data directory, so
# memory cards and savestates written from the pod stay readable outside it.
# It also lets pulseaudio run at all: it refuses to start as root.
# Xvfb wants /tmp/.X11-unix and cannot create it as a non-root user, which it
# reports as "_XSERVTransmkdir: ERROR: euid != 0". Creating it here with the
# usual sticky-bit permissions removes the message; the server worked either
# way, since it also listens on an abstract socket.
RUN mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix

RUN mkdir -p /home/ubuntu && chown -R ubuntu:ubuntu /home/ubuntu /opt/zs1
USER ubuntu
ENV HOME=/home/ubuntu

WORKDIR /opt/zs1
ENTRYPOINT ["/opt/zs1/entrypoint.sh"]
