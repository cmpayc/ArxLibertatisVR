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

#ifndef ARX_GRAPHICS_D3D11_D3D11WINDOW_H
#define ARX_GRAPHICS_D3D11_D3D11WINDOW_H

class Renderer;

/*!
 * Window system interface for the Direct3D 11 renderer.
 *
 * This header does not include any Windows / Direct3D headers so it can be used from
 * code that is part of the unity build.
 */
namespace d3d11 {

//! Create a new Direct3D 11 renderer, the device is created by \ref createDevice()
Renderer * createRenderer();

/*!
 * Create the device on a specific adapter (VR: the one the headset is connected to), before \ref createDevice()
 *
 * \param luid The LUID of the adapter (LowPart in the low 32 bits, HighPart in the high 32 bits)
 */
void setRequiredAdapter(unsigned long long luid);

//! \return true if the renderer is a Direct3D 11 renderer (it can fall back to OpenGL)
bool isDirect3D11(const Renderer * renderer);

/*!
 * Create the Direct3D 11 device and swap chain for a native window.
 *
 * \param window a HWND
 *
 * \return the number of MSAA samples or 0 if the device could not be created.
 */
int createDevice(Renderer & renderer, void * window, int maxMSAALevel);

void setVSync(Renderer & renderer, int vsync);

void present(Renderer & renderer);

} // namespace d3d11

#endif // ARX_GRAPHICS_D3D11_D3D11WINDOW_H
