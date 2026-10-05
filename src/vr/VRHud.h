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

#ifndef ARX_VR_VRHUD_H
#define ARX_VR_VRHUD_H

#include "math/Rectangle.h"

class Entity;

/*!
 * Areas of the virtual screen (window pixels, top-left origin) that are shown separately in VR while in-game:
 * the gauges on the off hand wrist, the inventory extending from it, the mini map on the main hand wrist and the
 * remaining 2D interface (book, notes, containers) as a panel in front of the player. The rest of the HUD is not
 * shown. Empty rectangles are not shown.
 */
struct VRHudAreas {

	Rect screen;    //!< Book, notes and containers
	Rect health;    //!< Health gauge
	Rect mana;      //!< Mana gauge
	Rect inventory; //!< Player inventory, while open
	Rect map;       //!< Mini map
	Rect loot;      //!< Inventory of a dead NPC, shown lying on the body
	const Entity * lootEntity = nullptr; //!< The dead NPC

	float hudScale = 1.f; //!< Screen pixels per interface pixel of the gauges and the inventory
	float mapScale = 1.f; //!< Screen pixels per interface pixel of the mini map

};

//! \return the areas laid out by the interface drawn in this frame
[[nodiscard]] VRHudAreas getVRHudAreas();

#endif // ARX_VR_VRHUD_H
