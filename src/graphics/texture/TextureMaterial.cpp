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

#include "graphics/texture/TextureMaterial.h"

// Highlights and reflections for the materials named in texture file names, the first matching tag wins
static const struct {
	std::string_view tag;
	TextureMaterial material;
} surfaceMaterials[] = {
	{ "lava",    { 0.f, 1.f, 0.f, 0.f, 1.f } },
	{ "water",   { 1.f, 96.f, 0.f, 1.f } },
	{ "wet",     { 0.8f, 64.f, 0.f, 0.f } },
	{ "blood",   { 0.7f, 64.f, 0.3f, 0.f } },
	{ "mud",     { 0.5f, 48.f, 0.f, 0.f } },
	// Crystals and gems reflect (only level geometry) and look clear and deep, before "glass" for "[glass]_quartz"
	{ "quartz",  { 0.6f, 128.f, 0.f, 0.3f, 0.f, 0.f, 1.f } },
	{ "crystal", { 0.6f, 128.f, 0.f, 0.3f, 0.f, 0.f, 1.f } },
	{ "diamond", { 0.6f, 128.f, 0.f, 0.3f, 0.f, 0.f, 1.f } },
	{ "gems",    { 0.6f, 128.f, 0.f, 0.3f, 0.f, 0.f, 1.f } },
	// The crystal mirrors of the temple of illusion ("[glass]_mirror"), polished: sharper reflections
	{ "mirror",  { 0.6f, 128.f, 0.f, 0.6f, 0.f, 0.f, 1.f } },
	{ "glass",   { 1.f, 128.f, 0.f, 0.f } },
	{ "vitrail", { 1.f, 128.f, 0.f, 0.f } },
	{ "ice",     { 0.3f, 96.f, 0.f, 0.2f, 0.f, 1.f } },
	{ "marble",  { 0.6f, 64.f, 0.f, 0.f } },
	{ "rust",    { 0.3f, 16.f, 1.f, 0.f } },
	{ "metal",   { 0.9f, 40.f, 1.f, 0.f } },
	{ "iron",    { 0.8f, 32.f, 1.f, 0.f } },
	{ "weapon",  { 0.9f, 40.f, 1.f, 0.f } },
	// Armor of characters (also the player): steel and brass parts shine and reflect, straps, cloth and flesh have a
	// leather sheen, before "mithril" for "npc_human_chainmail_mithril_body"
	{ "plate",        { 1.f, 48.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.12f, 1.f, 1.5f } },
	{ "chainmail",    { 0.8f, 32.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.12f, 1.f, 1.5f } },
	{ "ylside",       { 1.f, 48.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.12f, 1.f, 1.5f } },
	// Leather armor, only its bright grey buckles and studs are steel
	{ "leather",      { 1.f, 48.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.4f, 1.f, 1.3f } },
	{ "leatherblack", { 1.f, 48.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.4f, 1.f, 1.3f } },
	{ "leatherbrown", { 1.f, 48.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.4f, 1.f, 1.3f } },
	{ "gold",    { 1.f, 48.f, 1.f, 0.f } },
	{ "mithril", { 1.f, 48.f, 1.f, 0.f } },
	{ "silver",  { 1.f, 48.f, 1.f, 0.f } },
	{ "sword",   { 0.9f, 40.f, 1.f, 0.f } },
	{ "shield",  { 0.6f, 32.f, 1.f, 0.f } },
	{ "stone",   { 0.6f, 48.f, 0.f, 0.f } },
	{ "rock",    { 0.5f, 40.f, 0.f, 0.f } },
	{ "bone",    { 0.4f, 32.f, 0.f, 0.f } },
	{ "flesh",   { 0.2f, 16.f, 0.f, 0.f } },
	{ "wood",    { 0.15f, 10.f, 0.f, 0.f } },
	{ "woodl",   { 0.15f, 10.f, 0.f, 0.f } },
	{ "fabric",  { 0.f, 1.f, 0.f, 0.f } },
	{ "sand",    { 0.f, 1.f, 0.f, 0.f } },
	{ "soil",    { 0.f, 1.f, 0.f, 0.f } },
	{ "gravel",  { 0.f, 1.f, 0.f, 0.f } },
	{ "earth",   { 0.f, 1.f, 0.f, 0.f } },
	{ "dust",    { 0.f, 1.f, 0.f, 0.f } },
	{ "straw",   { 0.f, 1.f, 0.f, 0.f } },
	{ "plant",   { 0.f, 1.f, 0.f, 0.f } },
	{ "hair",    { 0.f, 1.f, 0.f, 0.f } },
	{ "fur",     { 0.f, 1.f, 0.f, 0.f } },
	{ "clothe",  { 0.f, 1.f, 0.f, 0.f } },
};

// Highlights and reflections of textures without a known material
static const TextureMaterial defaultMaterial = { 0.3f, 32.f, 0.f, 0.f };

TextureMaterial guessTextureMaterial(std::string_view name) {

	if(name.empty()) {
		return TextureMaterial();
	}

	// Only match whole words like "[metal]" or "_wet01", not "ice" in "notice"
	auto isLetter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
	for(const auto & material : surfaceMaterials) {
		for(size_t pos = name.find(material.tag); pos != std::string_view::npos;
		    pos = name.find(material.tag, pos + 1)) {
			size_t end = pos + material.tag.size();
			if((pos == 0 || !isLetter(name[pos - 1])) && (end == name.size() || !isLetter(name[end]))) {
				return material.material;
			}
		}
	}

	return defaultMaterial;
}
