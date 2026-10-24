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

#ifndef ARX_IO_LOG_ANDROIDLOGGER_H
#define ARX_IO_LOG_ANDROIDLOGGER_H

#include "io/log/LogBackend.h"

#ifdef __ANDROID__

namespace logger {

/*!
 * Logger that writes to the Android system log (logcat), with the tag "arx".
 */
class AndroidLog final : public Backend {

public:

	void log(const Source & file, int line, Logger::LogLevel level, std::string_view str) override;

};

} // namespace logger

#endif // __ANDROID__

#endif // ARX_IO_LOG_ANDROIDLOGGER_H
