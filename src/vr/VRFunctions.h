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

#ifndef ARX_VR_VRFUNCTIONS_H
#define ARX_VR_VRFUNCTIONS_H

/*
 * All OpenXR functions are called through pointers so that a runtime can be loaded directly, bypassing the
 * system loader (see VRSystem.cpp). The project is built with XR_NO_PROTOTYPES, so these pointers use the
 * names of the API functions.
 */

#include <openxr/openxr.h>

#define ARX_XR_GLOBAL_FUNCTIONS(X) \
	X(xrEnumerateInstanceExtensionProperties) \
	X(xrCreateInstance)

#define ARX_XR_INSTANCE_FUNCTIONS(X) \
	X(xrResultToString) \
	X(xrDestroyInstance) \
	X(xrGetInstanceProperties) \
	X(xrGetSystem) \
	X(xrGetSystemProperties) \
	X(xrEnumerateViewConfigurationViews) \
	X(xrCreateSession) \
	X(xrDestroySession) \
	X(xrBeginSession) \
	X(xrEndSession) \
	X(xrCreateReferenceSpace) \
	X(xrDestroySpace) \
	X(xrLocateSpace) \
	X(xrEnumerateSwapchainFormats) \
	X(xrCreateSwapchain) \
	X(xrDestroySwapchain) \
	X(xrEnumerateSwapchainImages) \
	X(xrAcquireSwapchainImage) \
	X(xrWaitSwapchainImage) \
	X(xrReleaseSwapchainImage) \
	X(xrPollEvent) \
	X(xrWaitFrame) \
	X(xrBeginFrame) \
	X(xrEndFrame) \
	X(xrLocateViews) \
	X(xrStringToPath) \
	X(xrPathToString) \
	X(xrCreateActionSet) \
	X(xrDestroyActionSet) \
	X(xrCreateAction) \
	X(xrSuggestInteractionProfileBindings) \
	X(xrAttachSessionActionSets) \
	X(xrGetCurrentInteractionProfile) \
	X(xrSyncActions) \
	X(xrGetActionStateBoolean) \
	X(xrGetActionStateFloat) \
	X(xrGetActionStateVector2f) \
	X(xrGetActionStatePose) \
	X(xrCreateActionSpace) \
	X(xrApplyHapticFeedback) \
	X(xrStopHapticFeedback)

#define ARX_XR_DECLARE_FUNCTION(name) extern PFN_##name name;
extern PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr;
ARX_XR_GLOBAL_FUNCTIONS(ARX_XR_DECLARE_FUNCTION)
ARX_XR_INSTANCE_FUNCTIONS(ARX_XR_DECLARE_FUNCTION)
#undef ARX_XR_DECLARE_FUNCTION

#endif // ARX_VR_VRFUNCTIONS_H
