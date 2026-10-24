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

#ifndef ARX_VR_VRPERF_H
#define ARX_VR_VRPERF_H

#include <chrono>
#include <string>

/*!
 * Frame time statistics in VR: where the CPU time of a frame goes, and the GPU time of the Vulkan renderer.
 *
 * Averaged over one second, shown with the "show_perf" VR option and written to the log every 5 seconds.
 */
enum VRPerfSection {
	VRPerfWait,   //!< xrWaitFrame: waiting for the compositor (the frame was ready early)
	VRPerfUpdate, //!< Game logic, physics, animation
	VRPerfRender, //!< Recording the frame: the 3D scene and the interface
	VRPerfScene,  //!< The 3D scene only (part of VRPerfRender)
	VRPerfEyes,   //!< Finishing the scene and copying the eyes into the OpenXR images (part of VRPerfScene)
	VRPerfSubmit, //!< Copies into the OpenXR images, submitting, xrEndFrame
	// The OpenXR images (eyes and interface), parts of the sections above
	VRPerfImageAcquire, //!< xrAcquireSwapchainImage and xrWaitSwapchainImage
	VRPerfImageCopy,    //!< Recording and submitting the copy into the image
	VRPerfImageRelease, //!< xrReleaseSwapchainImage
	VRPerfSectionCount
};

//! Adds the time until it is destroyed to a section
class VRPerfTimer {

public:

	explicit VRPerfTimer(VRPerfSection section)
		: m_section(section)
		, m_start(std::chrono::steady_clock::now())
	{ }

	~VRPerfTimer();

	VRPerfTimer(const VRPerfTimer &) = delete;
	VRPerfTimer & operator=(const VRPerfTimer &) = delete;

private:

	VRPerfSection m_section;
	std::chrono::steady_clock::time_point m_start;

};

void addVRPerfTime(VRPerfSection section, std::chrono::steady_clock::duration duration);

//! Count a frame, once per frame after xrWaitFrame
void countVRPerfFrame();

//! The statistics of the last second, empty before the first second
const std::string & getVRPerfSummary();

#endif // ARX_VR_VRPERF_H
