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

#ifndef ARX_GRAPHICS_VULKAN_VULKANWINDOW_H
#define ARX_GRAPHICS_VULKAN_VULKANWINDOW_H

class Renderer;

/*!
 * Window system interface for the Vulkan renderer.
 *
 * This header does not include any Vulkan headers so it can be used from code that is part of the unity build.
 */
namespace vulkan {

//! Create a new Vulkan renderer, the device is created by \ref createDevice()
Renderer * createRenderer();

//! \return true if the renderer is a Vulkan renderer (it can fall back to another backend)
bool isVulkan(const Renderer * renderer);

/*!
 * Create the Vulkan instance, device and swapchain for an SDL window created with SDL_WINDOW_VULKAN.
 *
 * \param window a SDL_Window
 *
 * \return the number of MSAA samples or 0 if the device could not be created.
 */
int createDevice(Renderer & renderer, void * window, int maxMSAALevel);

void setVSync(Renderer & renderer, int vsync);

void present(Renderer & renderer);

//! Work done since the last call to \ref takeStats()
struct Stats {
	double gpuMilliseconds = 0.0; //!< Execution time of the submitted command buffers (0 without timestamps)
	double fenceWaitMilliseconds = 0.0; //!< CPU time spent waiting for older command buffers to finish
	double beginMilliseconds = 0.0; //!< CPU time starting command buffers, including the fence wait
	double endCommandsMilliseconds = 0.0; //!< CPU time in vkEndCommandBuffer
	double submitMilliseconds = 0.0; //!< CPU time in vkQueueSubmit
	double maxSubmitMilliseconds = 0.0; //!< The longest vkQueueSubmit
	unsigned submissions = 0;
	unsigned draws = 0;
	unsigned textureUploads = 0; //!< Batches of texture uploads submitted with a frame
	double uploadedMegabytes = 0.0; //!< Vertices, indices and constants copied into the ring buffers
};

Stats takeStats(Renderer & renderer);

} // namespace vulkan

#endif // ARX_GRAPHICS_VULKAN_VULKANWINDOW_H
