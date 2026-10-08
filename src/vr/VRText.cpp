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

#include "vr/VRText.h"

namespace {

// '*' starts a new line. Messages without controls (or with interface elements VR does not show) are not replaced.
const std::string_view g_english = R"(
[system_tutorial_1]
string="Look around with your head and point at things with your right hand."

[system_tutorial_1_new]
string="Point at this message and press the right trigger to close it."

[system_tutorial_1_new2]
string="Tutorial messages can be found in the log section of your quest book (press 'B')."

[system_tutorial_2]
string="Grab objects with the grip to move or throw them. To put an object you hold directly into your inventory, press the trigger of that hand."

[system_tutorial_2_new]
string="Point at any fixed object and press the right grip to activate it."

[system_tutorial_2bis]
string="Press 'B' to open your quest book. Hold 'B' to open its spell pages."

[system_tutorial_2tris]
string="Press 'X' to open your inventory above your left wrist."

[system_tutorial_3bis]
string="Equip the bone: open your inventory ('X'), point at the bone and tap the right trigger."

[system_tutorial_3bis_new]
string="To draw your weapon, reach over your shoulder and press the grip. Bring it back over your shoulder to put it away."

[system_tutorial_3bis_new2]
string="Swing your weapon to strike: the faster and wider the swing, the harder you will strike."

[system_tutorial_3]
string="Point at any character and press the right grip to talk to them."

[system_tutorial_4]
string="Bring your weapon back over your shoulder to put it away.*Point at a dead body and press the right grip to open its inventory."

[system_tutorial_4_new]
string="Grab items with the grip and press the trigger of that hand to put them into your inventory.*To use an object, tap it in your inventory with the right trigger."

[system_tutorial_5]
string="Swing your weapon to fight. Without a weapon, make a fist (grip and trigger) and punch."

[system_tutorial_5bis]
string="When fighting, swing to hit. The faster and wider the swing, the harder you hit."

[system_tutorial_6]
string="You've just found your first runes. Put them in your book: tap them in your inventory with the right trigger."

[system_tutorial_7]
string="Crouch down (or click the right stick) and smash the wooden trap."

[system_tutorial_8]
string="Once you have found the rope, double-tap it in your inventory with the right trigger, then point at the elevator mechanism and press the trigger to tie it."

[system_tutorial_9]
string="Point at a spell icon in your book and press the right trigger to see its formula."

[system_tutorial_10]
string="Tap the right trigger to go into magic mode (your hand glows). Hold the trigger and draw the runes with your hand, one at a time, then tap the trigger again to cast the spell.*A double tap ends your last lasting spell."

[system_tutorial_10_bis]
string="Well done! Note that you can precast spells in order to cast them later. To precast a spell, hold the right grip as you draw it. You can precast up to 3 spells."

[system_tutorial_14]
string="Tap a Magic Scroll in your inventory with the right trigger to memorise the spell"

[system_tutorial_15]
string="Now the spell has been precast: its symbol floats above your right hand. To cast it, make a fist (grip and trigger) and move it the way the arrow next to the symbol points"

[system_tutorial_16]
string="When the stairs icon shows on your left wrist, jump ('A') to change level."

[system_tutorial_level_01]
string="Congratulations! Your character's level has increased (see the icon on your left wrist)! Open your book ('B') to allocate additional experience points."

[system_tutorial_level_02]
string="Add points by pointing at an attribute's icon and pressing the right trigger."

[system_tutorial_17]
string="To place an item, or stack of items directly into your inventory, grab it with the grip and press the trigger of that hand."

[system_tutorial_18]
string="Grab the stack of bread with the grip and press the trigger of that hand to put it into your inventory."

[tutorial_bag]
string="Point at the inventory belt and press the left grip to show what's inside all your bags."

[tutorial_lever]
string="To repair the mechanism, double-tap the lever you've just found in your inventory with the right trigger, then point at the lever base on the wall nearby and press the trigger."

[tutorial_stealth]
string="NPCs can hear you, so mind your footsteps. Click the left stick to sneak.*You can try throwing an item away from you and it might make hostile NPCs move towards it.*Note that the stealth icon shows up on your left wrist when you are hiding."
)";

} // anonymous namespace

std::string_view getVRLocalisation(std::string_view language) {
	return (language == "english") ? g_english : std::string_view();
}
