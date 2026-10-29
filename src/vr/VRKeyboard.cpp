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

#include "vr/VRKeyboard.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "core/Core.h"
#include "core/Localisation.h"
#include "graphics/Draw.h"
#include "graphics/Renderer.h"
#include "graphics/data/TextureContainer.h"
#include "graphics/font/Font.h"
#include "gui/Text.h"
#include "input/Input.h"
#include "input/Keyboard.h"
#include "input/Mouse.h"
#include "input/TextInput.h"
#include "scene/GameSound.h"
#include "vr/VRControls.h"

namespace {

enum KeyAction {
	KeyCharacter,
	KeyShift,
	KeySpace,
	KeyBackspace,
	KeyEnter,
	KeyClose
};

struct KeyboardKey {
	KeyAction action = KeyCharacter;
	char character = 0;
	Rectf rect;
};

// A QWERTY keyboard split over the two pages of the book, five key widths per row
const std::string_view LeftRows[] = { "12345", "qwert", "asdfg", "zxcvb" };
const std::string_view RightRows[] = { "67890", "yuiop", "hjkl-", "nm._" };
const int RowCount = 5;
const float RowWidth = 5.f; // keys

TextInputHandler * g_handler = nullptr; // The field the keyboard types into
std::string g_originalText; // The text of the field when the keyboard opened, for close
bool g_shift = false; // The next letter is upper case
TextureContainer * g_book = nullptr;

TextureContainer * getBookTexture() {
	if(!g_book) {
		g_book = TextureContainer::LoadUI("graph/interface/book/questbook");
	}
	return g_book;
}

//! The open book right of the menu window (which ends at 341 of 640), as large as fits
Rectf getBookRect(const TextureContainer & texture) {
	float width = RATIO_X(285.f);
	float height = width * float(texture.m_size.y) / float(texture.m_size.x);
	if(height > RATIO_Y(460.f)) {
		width *= RATIO_Y(460.f) / height;
		height = RATIO_Y(460.f);
	}
	Vec2f center(RATIO_X(348.f) + width / 2.f, RATIO_Y(240.f));
	return Rectf(center - Vec2f(width, height) / 2.f, width, height);
}

std::vector<KeyboardKey> layoutKeys(const Rectf & book, const TextureContainer & texture) {

	// The writing areas of the pages, in texture pixels (as for the quest book notes, see Note::calculateLayout)
	float scale = book.width() / float(texture.m_size.x);
	Vec2f size = Vec2f(texture.m_size) * scale;
	Rectf left(book.topLeft() + Vec2f(40.f, 30.f) * scale, size.x / 2.f - 50.f * scale, size.y - 75.f * scale);
	Rectf right = left;
	right.move(Vec2f(left.width() + 20.f * scale, 0.f));

	std::vector<KeyboardKey> keys;
	auto addKey = [&](const Rectf & page, int row, float column, float width, KeyAction action, char character) {
		float unitX = page.width() / RowWidth;
		float unitY = page.height() / float(RowCount);
		Rectf rect(page.topLeft() + Vec2f(column * unitX, float(row) * unitY), width * unitX, unitY);
		float inset = std::min(unitX, unitY) * 0.06f;
		rect.left += inset;
		rect.top += inset;
		rect.right -= inset;
		rect.bottom -= inset;
		KeyboardKey key;
		key.action = action;
		key.character = character;
		key.rect = rect;
		keys.push_back(key);
	};

	for(int row = 0; row < int(std::size(LeftRows)); row++) {
		for(size_t i = 0; i < LeftRows[row].size(); i++) {
			addKey(left, row, float(i), 1.f, KeyCharacter, LeftRows[row][i]);
		}
		for(size_t i = 0; i < RightRows[row].size(); i++) {
			addKey(right, row, float(i), 1.f, KeyCharacter, RightRows[row][i]);
		}
	}
	addKey(right, 3, 4.f, 1.f, KeyBackspace, 0);
	addKey(left, 4, 0.f, 2.f, KeyShift, 0);
	addKey(left, 4, 2.f, 3.f, KeySpace, 0);
	addKey(right, 4, 0.f, 3.f, KeyEnter, 0);
	addKey(right, 4, 3.f, 2.f, KeyClose, 0);

	return keys;
}

std::string getLabel(const KeyboardKey & key) {
	switch(key.action) {
		case KeyCharacter: {
			char c = key.character;
			return std::string(1, g_shift ? char(std::toupper(static_cast<unsigned char>(c))) : c);
		}
		case KeyShift:     return std::string(getLocalised("system_vr_keyboard_shift", "Shift"));
		case KeySpace:     return std::string(getLocalised("system_vr_keyboard_space", "Space"));
		case KeyBackspace: return std::string(getLocalised("system_vr_keyboard_backspace", "Del"));
		case KeyEnter:     return std::string(getLocalised("system_vr_keyboard_enter", "Enter"));
		case KeyClose:     return std::string(getLocalised("system_vr_keyboard_close", "Close"));
	}
	return std::string();
}

void pressKey(const KeyboardKey & key) {

	ARX_SOUND_PlayMenu(g_snd.MENU_CLICK);

	KeyModifiers none = { };
	switch(key.action) {
		case KeyCharacter: {
			char c = key.character;
			if(g_shift) {
				c = char(std::toupper(static_cast<unsigned char>(c)));
				g_shift = false;
			}
			g_handler->newText(std::string_view(&c, 1));
			break;
		}
		case KeyShift: {
			g_shift = !g_shift;
			break;
		}
		case KeySpace: {
			g_handler->newText(" ");
			break;
		}
		case KeyBackspace: {
			g_handler->keyPressed(Keyboard::Key_Backspace, none);
			break;
		}
		case KeyEnter: {
			// Leaves the field (see TextInputWidget::keyPressed)
			g_handler->keyPressed(Keyboard::Key_Enter, none);
			break;
		}
		case KeyClose: {
			// Restore the text: select all of it and replace it
			KeyModifiers control = { };
			control.control = true;
			g_handler->keyPressed(Keyboard::Key_A, control);
			g_handler->newText(g_originalText);
			g_handler->keyPressed(Keyboard::Key_Escape, none);
			break;
		}
	}

}

} // anonymous namespace

bool updateVRKeyboard() {

	TextInputHandler * handler = (isVRMotionControlled() && GInput) ? GInput->getTextInputHandler() : nullptr;
	if(handler != g_handler) {
		g_handler = handler;
		g_shift = false;
		g_originalText = handler ? std::string(handler->enteredText()) : std::string();
	}
	TextureContainer * texture = g_handler ? getBookTexture() : nullptr;
	if(!texture) {
		return false;
	}

	Rectf book = getBookRect(*texture);
	Vec2f mouse = Vec2f(GInput->getMousePosition());
	if(!book.contains(mouse)) {
		return false;
	}

	if(GInput->getMouseButtonNowPressed(Mouse::Button_0)) {
		for(const KeyboardKey & key : layoutKeys(book, *texture)) {
			if(key.rect.contains(mouse)) {
				pressKey(key);
				break;
			}
		}
	}

	return true;
}

void drawVRKeyboard() {

	if(!g_handler || !isVRMotionControlled() || !GInput->getTextInputHandler() || !hFontControls) {
		return;
	}
	TextureContainer * texture = getBookTexture();
	if(!texture) {
		return;
	}

	UseRenderState state(render2D());
	UseTextureState textureState(TextureStage::FilterLinear, TextureStage::WrapClamp);

	Rectf book = getBookRect(*texture);
	EERIEDrawBitmap(book, 0.f, texture, Color::white);

	// Ink on the pages: the keys are drawn as boxes, the one pointed at darker
	const Color ink = Color(25, 15, 5);
	// The font is thin in the headset: the labels are drawn bold, several times a little apart
	const s32 boldness = std::max(s32(1), s32(hFontControls->getLineHeight() / 16));
	Vec2f mouse = Vec2f(GInput->getMousePosition());
	for(const KeyboardKey & key : layoutKeys(book, *texture)) {
		bool hovered = key.rect.contains(mouse);
		bool active = (key.action == KeyShift && g_shift);
		Color background = hovered ? Color(90, 60, 25, 170) : active ? Color(90, 60, 25, 140) : Color(90, 60, 25, 90);
		EERIEDrawBitmap(key.rect, 0.f, nullptr, background);
		std::string label = getLabel(key);
		Vec2i textSize = hFontControls->getTextSize(label).size();
		Vec2i position = Vec2i(key.rect.center()) - textSize / s32(2);
		for(s32 y = 0; y <= boldness; y++) {
			for(s32 x = 0; x <= boldness; x++) {
				hFontControls->draw(position + Vec2i(x, y) - Vec2i(boldness / 2), label, ink);
			}
		}
	}

}
