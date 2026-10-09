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

#ifndef ARX_VR_VRKEYBOARD_H
#define ARX_VR_VRKEYBOARD_H

/*!
 * On-screen keyboard for text fields in the menus (VR, user request: naming a saved game needs a keyboard).
 *
 * While a text field has the focus in VR, an open book is shown next to the menu with a QWERTY keyboard split
 * over its two pages (letters, digits, '.', '-', '_', space, shift, backspace, enter and close). The keys are
 * clicked with the laser pointer and type into the focused field like a real keyboard. Enter keeps the text,
 * close restores the text the field had when the keyboard opened; both leave the field.
 */

/*!
 * Handle a click on the keyboard, once per menu frame before the menu is updated
 *
 * \return true while the pointer is over the keyboard: the menu must not handle the mouse then
 */
bool updateVRKeyboard();

//! Draw the keyboard over the menu, if it is shown
void drawVRKeyboard();

#endif // ARX_VR_VRKEYBOARD_H
