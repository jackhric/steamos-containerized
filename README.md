<p align="center">
  <img src="assets/header.png" alt="Steam Headless" width="100%">
</p>

# SteamOS™ Containerized

This repo allows you to run Steam as a headless Docker container, with emphasis on the Gamescope session that is utilized by the Steam Deck and Steam Machine.

## Quick Start



## Use case
Very useful as a Docker container. Since the primary use case is for Gamescope sessions, you no longer have to finangle with game windows or similar to get up and running. Everything just (mostly) works! Prioritizes couch-potato gameplay!

Second big plus is that it allows you to leave windows open in your DE, and have absolutely no idea about games running in the background. If someone wants to play games, you no longer have to close out of your sessions / windows to play. It's like nothing ever happened. 

## Self-contained display stack
The container never uses a display server from the host. It runs its own compositor and Xwayland(s); no host X11 or Wayland socket, `DISPLAY`, or `XAUTHORITY` is needed or honoured, and none should be mounted or passed in.

Because the container uses `network_mode: host`, it shares the host's *abstract* Unix sockets, so a bare `DISPLAY=:0` inside it can silently reach the host's X server. Code in this repo therefore addresses X displays by socket path (`/tmp/.X11-unix/XN`, private to the container), never by `:N`.

## Credits
This project stands on the shoulders of Wolf(https://github.com/games-on-whales/wolf), a headless multi-seat game streaming solution. 

More README details will be released in subsequent commits. Review the Docker Compose file to get started.