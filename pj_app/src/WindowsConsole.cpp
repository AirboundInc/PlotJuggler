// SPDX-License-Identifier: MPL-2.0
#include "WindowsConsole.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

namespace PJ {
namespace {

/// True when the process inherited no usable handle for this stream, i.e. the
/// parent did not redirect it to a pipe, file or console.
bool streamIsUnset(DWORD std_handle_id) {
  const HANDLE handle = GetStdHandle(std_handle_id);
  return handle == nullptr || handle == INVALID_HANDLE_VALUE || GetFileType(handle) == FILE_TYPE_UNKNOWN;
}

}  // namespace

void attachParentConsole() {
  // Fails when started from Explorer or a shortcut (no parent console): the
  // normal GUI launch, where staying console-less is the point.
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
    return;
  }
  FILE* reopened = nullptr;
  if (streamIsUnset(STD_OUTPUT_HANDLE)) {
    freopen_s(&reopened, "CONOUT$", "w", stdout);
  }
  if (streamIsUnset(STD_ERROR_HANDLE)) {
    freopen_s(&reopened, "CONOUT$", "w", stderr);
  }
}

}  // namespace PJ
