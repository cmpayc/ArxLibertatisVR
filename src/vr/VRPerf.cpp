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

#include "vr/VRPerf.h"

#include <iomanip>
#include <sstream>

#include "Configure.h"

#include "graphics/Renderer.h"
#include "io/log/Logger.h"

#if ARX_HAVE_VULKAN
#include "graphics/vulkan/VulkanWindow.h"
#endif

namespace {

const std::chrono::seconds SummaryPeriod(1);
const int LogEverySummaries = 5;

std::chrono::steady_clock::duration g_sections[VRPerfSectionCount];
unsigned g_frames = 0;
std::chrono::steady_clock::time_point g_periodStart;
bool g_started = false;
int g_summaries = 0;
std::string g_summary;

double toMilliseconds(std::chrono::steady_clock::duration duration) {
	return std::chrono::duration<double, std::milli>(duration).count();
}

} // anonymous namespace

VRPerfTimer::~VRPerfTimer() {
	addVRPerfTime(m_section, std::chrono::steady_clock::now() - m_start);
}

void addVRPerfTime(VRPerfSection section, std::chrono::steady_clock::duration duration) {
	g_sections[section] += duration;
}

void countVRPerfFrame() {

	auto now = std::chrono::steady_clock::now();
	if(!g_started) {
		g_started = true;
		g_periodStart = now;
		for(auto & section : g_sections) {
			section = std::chrono::steady_clock::duration::zero();
		}
		#if ARX_HAVE_VULKAN
		if(vulkan::isVulkan(GRenderer)) {
			vulkan::takeStats(*GRenderer);
		}
		#endif
		return;
	}

	g_frames++;

	auto elapsed = now - g_periodStart;
	if(elapsed < SummaryPeriod) {
		return;
	}

	double frames = double(g_frames);
	double frameTime = toMilliseconds(elapsed) / frames;

	std::ostringstream oss;
	oss << std::fixed << std::setprecision(1);
	oss << frames / std::chrono::duration<double>(elapsed).count() << " fps, frame " << frameTime << " ms"
	    << " | wait " << toMilliseconds(g_sections[VRPerfWait]) / frames
	    << ", update " << toMilliseconds(g_sections[VRPerfUpdate]) / frames
	    << ", render " << toMilliseconds(g_sections[VRPerfRender]) / frames
	    << " (scene " << toMilliseconds(g_sections[VRPerfScene]) / frames
	    << ", eyes " << toMilliseconds(g_sections[VRPerfEyes]) / frames << ")"
	    << ", submit " << toMilliseconds(g_sections[VRPerfSubmit]) / frames
	    << " | images: acquire " << toMilliseconds(g_sections[VRPerfImageAcquire]) / frames
	    << ", copy " << toMilliseconds(g_sections[VRPerfImageCopy]) / frames
	    << ", release " << toMilliseconds(g_sections[VRPerfImageRelease]) / frames;

	#if ARX_HAVE_VULKAN
	if(vulkan::isVulkan(GRenderer)) {
		vulkan::Stats stats = vulkan::takeStats(*GRenderer);
		oss << " | GPU " << stats.gpuMilliseconds / frames
		    << ", fence wait " << stats.fenceWaitMilliseconds / frames
		    << ", begin " << stats.beginMilliseconds / frames
		    << ", end " << stats.endCommandsMilliseconds / frames
		    << ", vkQueueSubmit " << stats.submitMilliseconds / frames << " (max " << stats.maxSubmitMilliseconds << ")"		    << " | " << std::setprecision(0) << double(stats.draws) / frames << " draws, "
		    << double(stats.submissions) / frames << " submits, "
		    << std::setprecision(2) << double(stats.textureUploads) / frames << " texture uploads, "
		    << std::setprecision(1) << stats.uploadedMegabytes / frames << " MB";
	}
	#endif

	g_summary = oss.str();

	if(++g_summaries % LogEverySummaries == 0) {
		LogInfo << "VR perf: " << g_summary;
	}

	g_frames = 0;
	g_periodStart = now;
	for(auto & section : g_sections) {
		section = std::chrono::steady_clock::duration::zero();
	}

}

const std::string & getVRPerfSummary() {
	return g_summary;
}
