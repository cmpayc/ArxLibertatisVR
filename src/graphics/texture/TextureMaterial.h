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

#ifndef ARX_GRAPHICS_TEXTURE_TEXTUREMATERIAL_H
#define ARX_GRAPHICS_TEXTURE_TEXTUREMATERIAL_H

#include <string_view>

//! Specular highlights and reflections of a surface, guessed from the material in the texture file name
struct TextureMaterial {
	float strength = 0.f;
	float exponent = 1.f; //!< Larger for sharper highlights
	float metallic = 0.f; //!< How much the texture color tints the highlights
	float reflectivity = 0.f; //!< Strength of the screen-space reflections, only water and ice reflect for now
	float glow = 0.f; //!< Glowing, flowing lava
	float ice = 0.f; //!< Deep, cold and sparkling ice on the bright, bluish parts of the texture
	float crystal = 0.f; //!< Like ice, but clear and on the whole texture
	//! Armor: grey parts brighter than this are steel and yellow ones brass, only they get the highlights and
	//! reflect, 0 if the whole texture is one material
	float metalParts = 0.f;
	float leather = 0.f; //!< Soft sheen of the parts of armor that are not metal (leather, cloth, flesh)
	float relief = 1.f; //!< Strength of the normal map of models, for engraved or embossed armor
};

//! Guess the material of a texture from tags like "[metal]" or "_wet01" in its file name
[[nodiscard]] TextureMaterial guessTextureMaterial(std::string_view name);

#endif // ARX_GRAPHICS_TEXTURE_TEXTUREMATERIAL_H
