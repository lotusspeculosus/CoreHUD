// Single source of truth for the CoreHUD version. Bump these for a release; the GitHub release tag must be
// "v<major>.<minor>.<patch>" so the auto-updater can compare versions.
#pragma once

#define COREHUD_VER_MAJOR 1
#define COREHUD_VER_MINOR 5
#define COREHUD_VER_PATCH 1
#define COREHUD_VERSION_STR "1.5.1"   // keep in sync with the three numbers above (checked in tests\test_update.cpp)

#ifndef RC_INVOKED
// build.bat writes build\buildinfo.h with the git commit and build date
#if defined(__has_include)
#if __has_include("buildinfo.h")
#include "buildinfo.h"
#endif
#endif
#ifndef COREHUD_BUILD_REV
#define COREHUD_BUILD_REV "dev"
#endif
#ifndef COREHUD_BUILD_DATE
#define COREHUD_BUILD_DATE __DATE__
#endif
#define COREHUD_WIDEN2(x) L##x
#define COREHUD_WIDEN(x) COREHUD_WIDEN2(x)
#define COREHUD_VERSION COREHUD_WIDEN(COREHUD_VERSION_STR)
#define COREHUD_BUILD COREHUD_WIDEN(COREHUD_BUILD_REV) L" · " COREHUD_WIDEN(COREHUD_BUILD_DATE)
#endif
