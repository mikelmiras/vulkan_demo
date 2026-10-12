Client code for the Wayland protocols the Linux Wayland backend (src/platform/platform_wayland.cpp) uses,
generated with wayland-scanner 1.24.0 from wayland-protocols (MIT; each file keeps its copyright notice):

    wayland-scanner client-header <protocol>.xml <name>-client-protocol.h
    wayland-scanner private-code  <protocol>.xml <name>-protocol.c

- stable/xdg-shell/xdg-shell.xml
- unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
- unstable/relative-pointer/relative-pointer-unstable-v1.xml
- unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
- stable/viewporter/viewporter.xml
- staging/fractional-scale/fractional-scale-v1.xml
- wayland.xml (the core protocol, from libwayland: its interfaces, as wayland-protocol.c, since the library is
  loaded at run time)

Kept here so building needs only libwayland-dev and libxkbcommon-dev, not the scanner and the protocol files.
