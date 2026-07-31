<p align="center">
  <img src="assets/header.png" alt="Steam Headless" width="100%">
</p>

# SteamOS™ Containerized

This repo allows you to run the SteamOS platform as a headless Docker container, and stream games to your devices (TV, phone, etc.) just as if you had a Steam Deck or Steam Machine. 

## Features
* Containerized platform: Keep using your desktop for other things while playing games, no more closing out of windows
* Moonlight compatibility built right in: stream with ultra-low latency
* Functional MangoHUD: monitor frametimes and other metrics easily right in SteamOS
* Gamescope session: fully navigatable with a controller, no more keyboards at the couch
* Seamless USB/IP integration: pass through devices (like the new 2026 Steam Controller) through your client to your host painlessly

## Use case
For those that have run a Windows or generic Linux gaming HTPC or streaming setup before, you know the pain. Your games' windows don't focus properly, you end up alt-tabbing 24/7, and there's always a keyboard plugged in case something goes wrong. Say goodbye to that! Since the primary focus of the project is running containerized SteamOS Gamescope sessions, everything just (mostly) works! This project brings the seamlessness of SteamOS right to any configuration, and lets you use your already existing hardware to play games instead of dropping ~$1K on a brand new Steam Machine. Use any old thin client or computer and stream this container right to the TV with minimal latency. 

Second big plus is that it allows you to leave windows open in your DE, and have absolutely no idea about games running in the background. If someone wants to play games, you no longer have to close out of your sessions / windows to play. It's like nothing ever happened. 

## Credits
The project builds on a variety of open source technologies and stands on the shoulders of giants.
* Wolf (https://github.com/games-on-whales/wolf), a headless multi-seat game streaming solution.
* Sunshine (https://github.com/LizardByte/Sunshine), extremely low latency streaming solution.
* Moonlight (https://github.com/moonlight-stream), awesome client for Sunshine (and previously NVIDIA's GameStream protocol).

More README details will be released in subsequent commits. Review the Docker Compose file to get started.
