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

#ifndef ARX_VR_VRGRAB_H
#define ARX_VR_VRGRAB_H

class Entity;

/*!
 * Grabbing, pulling and throwing objects with the hand in VR
 *
 * Both hands point at world items (highlighting them) and use the grip:
 *  - a short press puts the object into the inventory,
 *  - holding it grabs: objects within reach are attached to the hand, distant objects stay selected until
 *    the hand is flicked towards the player, then fly to the hand,
 *  - releasing the grip lets go of the object with the velocity of the hand (dropping or throwing it),
 *  - pressing the trigger while holding puts the object into the inventory.
 * Items dragged out of an inventory use the game's drag (g_draggedEntity) and are then held by the main hand
 * the same way. While pointing at an interface panel (inventory, book) the game's normal drag and drop is used.
 */

//! Track the dominant hand in the world, once per VR frame
void updateVRHandMotion();

/*!
 * Place or release the dragged entity in the world
 *
 * \return false if the game should handle the drag (e.g. while pointing at an inventory)
 */
bool updateVRDraggedEntity(Entity & entity);

/*!
 * Point at and grab world objects with both hands directly, without the game's drag (which supports only one
 * object and needs the cursor)
 *
 * Called once per game frame.
 */
void updateVRHandGrabs();

//! \return true if a hand points at an entity to grab it (to highlight it)
bool isVRGrabTarget(const Entity & entity);

/*!
 * \return true if the main hand grip is used to grab directly: pointing at a grabbable object, or holding one.
 *         The grip is then not a game click.
 */
bool isVRGripGrabbing();

//! \return true while the main hand holds, selects or pulls an object it grabbed directly (not the game drag)
bool isVRGrabbingDirectly();

//! \return true while the main hand holds, selects or pulls an object
bool isVRGrabbing();

//! \return true while the off hand holds, selects or pulls an object
bool isVROffHandGrabbing();

#endif // ARX_VR_VRGRAB_H
