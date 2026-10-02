// Firmware identity shown on the ABOUT page (menu -> CONFIG's neighbour,
// ABOUT) and nowhere else: nothing in the render path or on-wire protocol
// depends on it.
//
// FIRMWARE_VERSION is bumped by hand on every release - there is no scheme
// to derive it from, the first one is simply "1.0". FIRMWARE_BUILD is the
// git short hash (+ "-dirty" if the working tree has uncommitted changes),
// injected by build.sh through CMake so it is never stale and never edited
// by hand. The native test benches do not go through CMake, so they fall
// back to "unknown" below rather than needing a build flag of their own.
#ifndef VERSION_H
#define VERSION_H

#define FIRMWARE_VERSION "1.1"

#ifndef FIRMWARE_BUILD
#define FIRMWARE_BUILD "unknown"
#endif

#endif // VERSION_H
