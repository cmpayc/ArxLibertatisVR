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

#include "io/log/AndroidLogger.h"

#include <sstream>

#include <android/log.h>

namespace logger {

void AndroidLog::log(const Source & file, int line, Logger::LogLevel level, std::string_view str) {

	int priority = ANDROID_LOG_INFO;
	switch(level) {
		case Logger::Debug: priority = ANDROID_LOG_DEBUG; break;
		case Logger::Warning: priority = ANDROID_LOG_WARN; break;
		case Logger::Error: priority = ANDROID_LOG_ERROR; break;
		case Logger::Critical: priority = ANDROID_LOG_FATAL; break;
		default: break;
	}

	// logcat adds its own time and level, keep the source location
	std::ostringstream oss;
	format(oss, file, line, level, str);
	__android_log_write(priority, "arx", oss.str().c_str());
}

} // namespace logger
