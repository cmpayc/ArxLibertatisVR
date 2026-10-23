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

#ifndef ARX_GRAPHICS_VULKAN_VULKANEXTERNALDEVICE_H
#define ARX_GRAPHICS_VULKAN_VULKANEXTERNALDEVICE_H

#include <volk.h>

namespace vulkan {

/*!
 * Creates the Vulkan instance and device of the renderer for someone else, the VR runtime
 * (OpenXR XR_KHR_vulkan_enable2 requires the instance and device to be created through it)
 */
class ExternalDevice {

public:

	virtual ~ExternalDevice() = default;

	//! The highest Vulkan version that may be used
	virtual uint32_t getMaxApiVersion() const = 0;

	virtual VkResult createInstance(PFN_vkGetInstanceProcAddr getInstanceProcAddr, const VkInstanceCreateInfo & info,
	                                VkInstance & instance) = 0;

	//! The physical device that must be used, VK_NULL_HANDLE on failure
	virtual VkPhysicalDevice getPhysicalDevice(VkInstance instance) = 0;

	virtual VkResult createDevice(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkPhysicalDevice physicalDevice,
	                              const VkDeviceCreateInfo & info, VkDevice & device) = 0;

};

/*!
 * Create the device of the next Vulkan renderer through this object, nullptr to create it normally
 *
 * The object must stay alive until the renderer is created.
 */
void setExternalDevice(ExternalDevice * device);

} // namespace vulkan

#endif // ARX_GRAPHICS_VULKAN_VULKANEXTERNALDEVICE_H
