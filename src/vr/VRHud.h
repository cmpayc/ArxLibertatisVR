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
class TextureContainer;

/*!
 * Areas of the virtual screen (window pixels, top-left origin) that are shown separately in VR while in-game:
 * the gauges on the off hand wrist, the inventory extending from it, the mini map on the main hand wrist and the
 * remaining 2D interface (book, notes, containers) as a panel in front of the player. The rest of the HUD is not
 * shown. Empty rectangles are not shown.
 */
struct VRHudAreas {

	Rect screen;    //!< Containers and other open interfaces, in front
	Rect book;      //!< The book (with its bookmarks), held in a hand
	Rect note;      //!< An open note (or in-game book), held in a hand like the book
	int bookWidth = 0; //!< Width the book (with its bookmarks) has or would have: the scale of a held note
	Rect gauges;    //!< Health and mana gauges, next to each other
	Rect equipment; //!< Damaged equipment icons (weapon and armor status)
	Rect stealth;   //!< Stealth icon, while hidden (shown in the lower left of the view)
	Rect levelUp;   //!< Level up icon, while there are points to allocate
	Rect changeLevel; //!< Stairs icon, while the player can change level (by jumping)
	Rect inventory; //!< Player inventory, while open
	Rect map;       //!< Mini map
	Rect runes;     //!< Runes of the spell being drawn (main hand wrist)
	Rect loot;      //!< Inventory of a dead NPC, shown lying on the body
	Rect otherInventory; //!< Inventory of a container or a dead NPC, wherever it is shown (pointed without a beam)
	Rect subtitles; //!< Subtitles of in-game cutscenes, below the view
	Rect info;      //!< Names of pointed objects and notifications (top of the screen), above the view
	Rect label;     //!< Name of the pointed object in the world, shown above it
	const Entity * labelEntity = nullptr; //!< The pointed object
	const Entity * lootEntity = nullptr; //!< The dead NPC

	float hudScale = 1.f; //!< Screen pixels per interface pixel of the gauges and the inventory
	float mapScale = 1.f; //!< Screen pixels per interface pixel of the mini map

};

//! \return the areas laid out by the interface drawn in this frame
[[nodiscard]] VRHudAreas getVRHudAreas();

//! \return the gold purse above the open inventory while there is gold (tapped to select gold to give), else empty
[[nodiscard]] Rect getVRPurseArea();

//! \return the icon of the purse, shown at the end of the pointer while gold is selected to give
[[nodiscard]] TextureContainer * getVRGoldIcon();

#endif // ARX_VR_VRHUD_H
