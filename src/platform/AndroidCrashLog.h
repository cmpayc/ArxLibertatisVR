/*
 * Copyright 2026 Arx Libertatis VR contributors <kazakov.s.fl@gmail.com>
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ARX_PLATFORM_ANDROIDCRASHLOG_H
#define ARX_PLATFORM_ANDROIDCRASHLOG_H

#ifdef __ANDROID__

#include "io/fs/FilePath.h"

namespace platform {

/*!
 * Append the signal and the call stack of a crash to the log file
 *
 * The full crash handler needs a second process, which Android apps can't start. Without adb the system crash
 * report (tombstone) is not reachable, so the stack goes to the log in the shared folder. The addresses are
 * offsets into the libraries, for llvm-addr2line with the unstripped libmain.so of the same build.
 * The previous handler (Android's debuggerd) still runs afterwards, a second crash signal ends the process.
 */
void installAndroidCrashLog(const fs::path & logFile);

} // namespace platform

#endif // __ANDROID__

#endif // ARX_PLATFORM_ANDROIDCRASHLOG_H
