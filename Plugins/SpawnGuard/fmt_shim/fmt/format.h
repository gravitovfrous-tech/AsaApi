#pragma once

// ---------------------------------------------------------------------------
// SpawnGuard fmt shim
//
// AsaApi headers use fmt by themselves:
//   API/UE/Containers/UnrealString.h  ->  #include "fmt/format.h"
//   FString::Format(...)              ->  fmt::format(...)
// and every SendChatMessage / SendNotification goes through FString::Format.
//
// The plugin therefore needs fmt headers. This folder makes "fmt/format.h"
// resolve to the fmt copy that already ships with AsaApi
// (Logger/spdlog/fmt/bundled, header-only) - nothing to install, nothing to
// link, and no version clash with the fmt inside spdlog.
//
// Usage: add <plugin folder>\fmt_shim to
//   Project -> Properties -> C/C++ -> General -> Additional Include Directories
// BEFORE any folder that contains a real "fmt" directory
// (SpawnGuard.vcxproj already does this).
// ---------------------------------------------------------------------------

#ifndef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY
#endif

// Same switch spdlog uses for its bundled copy.
#ifndef FMT_USE_WINDOWS_H
#define FMT_USE_WINDOWS_H 0
#endif

#include "Logger/spdlog/fmt/bundled/format.h"
