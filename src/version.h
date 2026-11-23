// Single source of truth for the RigSense version. Bump these for a release; the GitHub release tag must be
// "v<major>.<minor>.<patch>" so the auto-updater can compare versions.
#pragma once

#define RIGSENSE_VER_MAJOR 1
#define RIGSENSE_VER_MINOR 2
#define RIGSENSE_VER_PATCH 0
#define RIGSENSE_VERSION_STR "1.2.0"

#ifndef RC_INVOKED
// build.bat writes build\buildinfo.h with the git commit and build date
#if defined(__has_include)
#if __has_include("buildinfo.h")
#include "buildinfo.h"
#endif
#endif
#ifndef RIGSENSE_BUILD_REV
#define RIGSENSE_BUILD_REV "dev"
#endif
#ifndef RIGSENSE_BUILD_DATE
#define RIGSENSE_BUILD_DATE __DATE__
#endif
#define RIGSENSE_WIDEN2(x) L##x
#define RIGSENSE_WIDEN(x) RIGSENSE_WIDEN2(x)
#define RIGSENSE_VERSION RIGSENSE_WIDEN(RIGSENSE_VERSION_STR)
#define RIGSENSE_BUILD RIGSENSE_WIDEN(RIGSENSE_BUILD_REV) L" · " RIGSENSE_WIDEN(RIGSENSE_BUILD_DATE)
#endif
