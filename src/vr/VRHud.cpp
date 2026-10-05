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

#include "vr/VRHud.h"

#include <cmath>

#include "cinematic/CinematicController.h"
#include "core/Config.h"
#include "core/Core.h"
#include "game/Player.h"
#include "gui/CinematicBorder.h"
#include "gui/Hud.h"
#include "gui/Interface.h"
#include "gui/Menu.h"
#include "gui/MiniMap.h"
#include "gui/book/Book.h"
#include "gui/hud/PlayerInventory.h"
#include "gui/hud/SecondaryInventory.h"

extern bool SHOW_INGAME_MINIMAP;

namespace {

Rect toPixels(const Rectf & area) {
	return Rect(s32(std::floor(area.left)), s32(std::floor(area.top)),
	            s32(std::ceil(area.right)), s32(std::ceil(area.bottom)));
}

Rect merge(const Rect & a, const Rect & b) {
	if(a.empty()) {
		return b;
	}
	if(b.empty()) {
		return a;
	}
	return a | b;
}

} // anonymous namespace

VRHudAreas getVRHudAreas() {
	
	VRHudAreas areas;
	areas.hudScale = g_hudRoot.getScale();
	areas.mapScale = minSizeRatio();
	
	if(ARXmenu.mode() != Mode_InGame) {
		return areas;
	}
	
	if(config.vr.showHud) {
		// Debugging: the whole interface in front, as on a monitor
		areas.screen = g_size;
		return areas;
	}
	
	if(player.Interface & INTER_PLAYERBOOK) {
		// The bookmarks stick out of the book
		Rectf book = g_playerBook.getArea();
		float margin = 24.f * g_hudRoot.getScale();
		book = Rectf(book.left - margin, book.top - margin, book.right + margin, book.bottom + margin);
		areas.screen = merge(areas.screen, toPixels(book));
	}
	if(g_note.isOpen()) {
		areas.screen = merge(areas.screen, toPixels(g_note.area()));
	}
	if(g_secondaryInventoryHud.isVisible()) {
		// Without the slide-in animation, which moves the contents instead
		Rectf container = g_secondaryInventoryHud.rect();
		container.move(-container.left, 0.f);
		areas.screen = merge(areas.screen, toPixels(container));
	}
	
	bool cinematic = !cinematicIsStopped() || cinematicBorder.isActive();
	if(cinematic || player.lifePool.current <= 0.f) {
		return areas;
	}
	
	if(player.Interface & INTER_LIFE_MANA) {
		areas.health = toPixels(g_hudRoot.healthGaugeArea());
		areas.mana = toPixels(g_hudRoot.manaGaugeArea());
	}
	
	if(player.Interface & (INTER_INVENTORY | INTER_INVENTORYALL)) {
		areas.inventory = toPixels(g_playerInventoryHud.openArea());
	}
	
	if(SHOW_INGAME_MINIMAP && !BLOCK_PLAYER_CONTROLS && !(player.Interface & INTER_PLAYERBOOK)) {
		areas.map = MiniMap::playerMiniMapArea();
	}
	
	return areas;
}
