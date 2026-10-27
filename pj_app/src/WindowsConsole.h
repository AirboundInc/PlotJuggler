// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace PJ {

/// On Windows, plotjuggler4.exe is a GUI-subsystem executable, so it gets no
/// console of its own. When it is launched from a terminal, this attaches to
/// that terminal and points any unset stdout/stderr at it, so CLI modes
/// (--help, --validate-plugins, ...) and Qt logging stay visible. Streams that
/// are already redirected to a pipe or file are left alone. Windows-only:
/// compiled into the Windows build alone.
///
/// Call first thing in main(), before anything writes to stdout/stderr.
/// The terminal does not wait for a GUI process, so output can land after the
/// next prompt; scripts that need the exit code must pipe the output.
void attachParentConsole();

}  // namespace PJ
