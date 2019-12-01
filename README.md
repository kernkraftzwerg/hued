# hued
An SSDP responder daemon for Philips Hue devices (real or emulated) that are not reacheable by multicast UDP.

# Problem
If you are using an Amazon Echo for directly controlling Philips Hue devices (real or emulated) then only devices in the same
subnet can be found. Also your docker containers for node-red or HA-bridge only work if the containers are startet with "--network=host", which disables all port mapping and port blocking features of docker.

# Solution
This little daemon just handles the SSDP traffic for one Hue device, which can be located wherever you want as long it is
accessible via TCP network on port 80.

# Building
    cd Release
    make
For building you need gcc, libpthread and libboost. Runtime dependencies are glibc and libpthread.

# Installation and Running
Just copy hued to e.g. /usr/local/bin and start it with one argument server:port of your Hue bridge. Other ports than 80
may not work. For starting as service you need a start script for your init system; the following is for systemd:
    
    # hued@.service
    [Unit]
    Description=HUE ssdp responder
    After=network-online.target
    Wants=network-online.target

    [Service]
    ExecStart=/usr/local/bin/hued %i
    Restart=always

    [Install]
    WantedBy=multi-user.target
    
Please note the "@" at the end of the service name, which thells systemd that the service needs one parameter. Enable and
start it with

    systemctl enable --now hued@my-bridge:80
