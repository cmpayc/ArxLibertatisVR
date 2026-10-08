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

#ifndef ARX_VR_VRTEXT_H
#define ARX_VR_VRTEXT_H

#include <string_view>

/*!
 * Game texts that name keyboard and mouse controls (the tutorial messages), rewritten for the VR controllers.
 *
 * In the localisation format, loaded over the game's texts when VR is enabled (see loadLocalisations).
 * \return the texts for a language, empty if there are none
 */
[[nodiscard]] std::string_view getVRLocalisation(std::string_view language);

#endif // ARX_VR_VRTEXT_H
