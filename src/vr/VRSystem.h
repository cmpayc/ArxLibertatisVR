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

#ifndef ARX_VR_VRSYSTEM_H
#define ARX_VR_VRSYSTEM_H

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <openxr/openxr.h>

#include "game/GameTypes.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "math/Rectangle.h"
#include "math/Types.h"
#include "vr/VRInput.h"

class Texture;

struct VRHudAreas;


/*!
 * OpenXR integration: instance, OpenGL session, stereo swapchains and the frame loop.
 *
 * Lifetime:
 *  - initInstance() once at startup (no GL context needed)
 *  - createSession() / destroySession() follow the OpenGL context (onRendererInit / onRendererShutdown)
 *  - each frame: pollEvents(), beginFrame(), render into the eye images, endFrame()
 */
class VRSystem {

public:

	static const size_t EyeCount = 2;

	VRSystem();
	~VRSystem();

	VRSystem(const VRSystem &) = delete;
	VRSystem & operator=(const VRSystem &) = delete;

	//! \return true if VR was enabled in the config or on the command line
	static bool isRequested();

	bool initInstance();
	void shutdown();

	//! Create the session for the OpenGL context that is current on the calling thread
	bool createSession();
	void destroySession();

	//! Process pending OpenXR events, must be called once per frame
	void pollEvents();

	[[nodiscard]] bool hasSession() const { return m_session != XR_NULL_HANDLE; }
	[[nodiscard]] bool isSessionRunning() const { return m_sessionRunning; }
	[[nodiscard]] bool isFocused() const { return m_sessionState == XR_SESSION_STATE_FOCUSED; }
	[[nodiscard]] bool isExitRequested() const { return m_exitRequested; }
	/*!
	 * Whether the game should pause since the last call: the session lost input focus (headset taken off, runtime
	 * menu) or the headset reconnected
	 */
	[[nodiscard]] bool consumePauseRequest() { return std::exchange(m_pauseRequested, false); }

	/*!
	 * Wait for and begin the next frame and locate the eye views.
	 * endFrame() must be called after this returns, even if it returns false.
	 *
	 * \return true if the application should render the eye images this frame
	 */
	bool beginFrame();
	void endFrame();

private:

	//! Locate the eye views for the frame, false if the headset is not tracked
	bool locateViews(XrTime now);
	//! Request a pause when the headset or the controllers are gone for a while
	void updatePresence(bool headsetLocated, XrTime now);

public:

	[[nodiscard]] bool isFrameActive() const { return m_frameActive; }
	[[nodiscard]] Vec2i getEyeSize() const { return m_eyeSize; }
	//! Predicted duration of the current frame in seconds
	[[nodiscard]] float getFrameDuration() const {
		return std::min(float(m_frameState.predictedDisplayPeriod) * 1e-9f, 0.1f);
	}
	[[nodiscard]] const XrView & getView(size_t eye) const { return m_views[eye]; }

	//! \return true between beginFrame() and endFrame() if the eye images should be rendered
	[[nodiscard]] bool isRenderingFrame() const { return m_frameActive && m_shouldRender; }

	//! Set the eye views for the current frame, see \ref prepareVRCamera
	void setStereoViews(std::vector<OpenGLRenderer::StereoView> views);

	/*!
	 * Redirect rendering into the stereo render target, replicating draw calls into both eyes
	 *
	 * \return false if there are no eye views for this frame, in which case nothing was changed
	 */
	bool beginStereoRender();

	//! Resolve the stereo render target into the eye images and show the left eye in the window
	void endStereoRender(const Vec2i & windowSize);

	[[nodiscard]] bool hasRenderedStereo() const { return m_stereoRendered; }
	//! \return true between beginStereoRender and endStereoRender (drawing the 3D view into both eyes)
	[[nodiscard]] bool isRenderingStereo() const { return m_stereoActive; }

	/*!
	 * Redirect rendering into the virtual screen: a transparent window-sized layer shown as a quad in VR
	 *
	 * All 2D rendering (interface, menus, cinematics) should happen between beginUIRender() and endUIRender().
	 * The stereo 3D scene can be rendered in between (\ref beginStereoRender restores this target).
	 *
	 * \return false if there is no VR frame to render
	 */
	bool beginUIRender(const Vec2i & windowSize);

	/*!
	 * Submit the virtual screen to the headset and composite it into the window
	 *
	 * \param overScene true if the virtual screen is drawn over the 3D scene (in-game HUD) - it then follows
	 *                  the head, otherwise it stays in place like a cinema screen
	 */
	void endUIRender(const Vec2i & windowSize, bool overScene);

	[[nodiscard]] bool isUIActive() const { return m_uiActive; }
	[[nodiscard]] bool hasSubmittedUI() const { return m_uiSubmitted; }

	//! Show the window contents as the virtual screen, for frames rendered without \ref beginUIRender
	void submitWindowAsUI(const Vec2i & windowSize);

	//! Place the virtual screen in front of the user again
	void resetUIPlacement() { m_uiPlaced = false; }

	//! Leave the magic mode and forget the tapped items when the level is cleared (loading a save, changing level)
	void resetLevelState() {
		m_magicMode = false;
		m_triggerItem = EntityHandle();
		m_lastItemTap = EntityHandle();
	}
	
	[[nodiscard]] const VRInput & getInput() const { return m_input; }
	[[nodiscard]] VRInput & getInput() { return m_input; }
	
	//! \return true while the trigger is used to click on an interface panel (and not for magic)
	[[nodiscard]] bool isTriggerClicking() const { return m_clickByTrigger || m_triggerOnItem; }
	
	//! \return true if the laser pointer currently points at the virtual screen
	[[nodiscard]] bool isPointingAtUI() const { return m_pointerOnUI; }
	
	//! \return true while a trigger tap in the inventory uses the item (the game's use action, like a right click)
	[[nodiscard]] bool isUseTapPressed() const { return m_useTapFrames > 2; }

	//! \return true while the magic mode is on (toggled by a tap of the trigger in the world)
	[[nodiscard]] bool isMagicMode() const { return m_magicMode; }
	//! \return true if the spell drawn in the last magic mode is stored instead of cast (the grip held while drawing)
	[[nodiscard]] bool isMagicPrecast() const { return m_magicPrecast; }
	
	//! \return true while the book is open, held in a hand (see updateBookPanel)
	[[nodiscard]] bool isBookInHand() const { return m_bookOpen; }
	//! The hand holding the open book
	[[nodiscard]] VRHand getBookHand() const { return m_bookHand; }
	//! The hand pointing at panels and the world: the main hand, the other one while the main hand holds the book
	[[nodiscard]] VRHand getPointerHand() const;
	/*!
	 * Where the open book is held, in the tracking space: the center of its pages (held by a bottom corner, facing the
	 * user) and the size of its cover in metres
	 */
	bool getBookPose(Vec3f & position, glm::quat & orientation, Vec2f & coverSize) const;
	/*!
	 * The book as it is drawn in the interface (upside down: OpenGL images start at the bottom) and its size with the
	 * bookmarks in metres, nullptr while it is closed
	 */
	[[nodiscard]] Texture * getBookTexture(Vec2f & panelSize) const {
		panelSize = m_bookPanel.size;
		return (m_bookOpen && m_bookPanel.active) ? m_bookPanel.texture : nullptr;
	}
	/*!
	 * The open inventory at the wrist, drawn in the world like the book: its texture (upside down), where it is in the
	 * tracking space now (attached to the wrist) and its size in metres, false if it is not shown
	 */
	bool getInventoryPanel(Texture *& texture, Vec3f & position, glm::quat & orientation, Vec2f & size) const;

	//! Acquire the current swapchain image for an eye and return its OpenGL texture name
	unsigned acquireEyeImage(size_t eye);
	void releaseEyeImage(size_t eye);

	[[nodiscard]] const std::string & getRuntimeName() const { return m_runtimeName; }
	[[nodiscard]] const std::string & getSystemName() const { return m_systemName; }

private:

	struct EyeSwapchain {
		XrSwapchain handle = XR_NULL_HANDLE;
		std::vector<unsigned> images;
		bool acquired = false;
	};

	bool createSwapchains();
	void destroySwapchains();
	bool createStereoTarget();
	void destroyStereoTarget();
	bool createUITarget(const Vec2i & size);
	void destroyUITarget();
	void updateUIPlacement(bool followHead);
	//! Show a part of the virtual screen at the screen placement, see \ref updateUIPlacement
	void addScreenPanel(const Rect & area, bool interactive, bool keepPosition = false);
	//! Show a part of the virtual screen as a quad of the given size (metres per pixel)
	bool addUIPanel(const Rect & area, float pixelSize, const Vec3f & position, const glm::quat & orientation,
	                bool interactive);
	//! Attach the HUD to the wrists, see \ref VRHudAreas
	void updateWristPanels(const VRHudAreas & areas);
	//! Round backings, 30% transparent, for the icons at the wrist, painted into the interface image
	void drawWristIconBackings();
	//! Show the loot of a dead NPC like a sheet of paper lying on the body, see \ref VRHudAreas
	void updateLootPanel(const VRHudAreas & areas);
	//! Show the name of the pointed object above it, facing the user, see VRHudAreas
	//! Show icons in the view, following the head: the stealth icon, see \ref VRHudAreas
	void updateViewIconPanels(const VRHudAreas & areas);
	//! The open book held in a hand, see \ref VRHudAreas
	void updateBookPanel(const VRHudAreas & areas);
	void updateLabelPanel(const VRHudAreas & areas);
	//! \return false if nothing is drawn to the desktop window (see config.vr.desktopView)
	static bool isDesktopViewShown();
	void drawTextureToWindow(unsigned texture, const Vec2i & windowSize, bool clear);
	//! The panels of this frame as the left eye sees them, over the desktop view (like the headset)
	void drawPanelsToWindow(const Vec2i & windowSize);
	//! Draw into the window with a texture and premultiplied alpha, saving and restoring the renderer's state
	void drawToWindow(unsigned texture, const Vec2i & windowSize, bool clear, const std::function<void()> & draw);
	//! The panels of the in-game interface: the book, the screen, subtitles, the wrists, labels...
	void updateGamePanels();
	void updatePointer();
	unsigned acquireSwapchainImage(EyeSwapchain & swapchain);
	void releaseSwapchainImage(EyeSwapchain & swapchain);
	void handleSessionStateChange(XrSessionState state);
	bool check(XrResult result, const char * call) const;

	XrInstance m_instance;
	XrSystemId m_systemId;
	XrSession m_session;
	XrSpace m_stageSpace;
	XrSessionState m_sessionState;
	bool m_sessionRunning;
	bool m_exitRequested;

	std::array<EyeSwapchain, EyeCount> m_swapchains;
	std::array<XrView, EyeCount> m_views;
	std::array<XrCompositionLayerProjectionView, EyeCount> m_layerViews;
	Vec2i m_eyeSize;
	s64 m_colorFormat;
	unsigned m_blitFramebuffer;

	// Double-wide (left eye | right eye) multisampled render target and its resolve target
	std::vector<OpenGLRenderer::StereoView> m_stereoViews;
	unsigned m_stereoFramebuffer;
	unsigned m_stereoColor;
	unsigned m_stereoDepth;
	unsigned m_stereoResolveFramebuffer;
	unsigned m_stereoResolveColor;
	bool m_stereoActive;
	bool m_stereoRendered;
	unsigned m_stereoPreviousTarget;
	Vec2i m_stereoPreviousTargetSize;

	// Virtual screen (interface layer) render target and quad layer
	EyeSwapchain m_uiSwapchain;
	unsigned m_uiFramebuffer;
	unsigned m_uiColor;
	unsigned m_uiDepth;
	Vec2i m_uiSize;
	bool m_uiActive;
	bool m_uiSubmitted;
	bool m_uiRendered; // The virtual screen was rendered this frame (even if not shown)
	bool m_uiPlaced;
	bool m_uiFollowHead;
	bool m_uiTurning;
	float m_uiYaw; // radians, OpenXR stage space
	Vec3f m_uiAnchor; // head position the screen is placed relative to, OpenXR stage space
	XrPosef m_uiPose;
	XrExtent2Df m_uiExtent;
	
	// Parts of the virtual screen shown as separate quad layers
	struct UIPanel {
		Rect area; // Window pixels, top-left origin
		XrPosef pose;
		XrExtent2Df extent;
		bool interactive; // The laser pointer can click on it
		bool useOnTap = false; // A short trigger click uses the item (right click), holding it drags
		bool noBeam = false; // No beam is drawn to it, the cursor is enough (the wrists, the book)
		bool hidden = false; // Not shown as a quad layer, only pointed at (drawn in the world: the book)
	};
	std::vector<UIPanel> m_uiPanels; // Submitted in this frame
	std::vector<UIPanel> m_visiblePanels; // Shown in the last frame
	
	// Controllers and the laser pointer (dominant hand) emulating the mouse on the virtual screen
	VRInput m_input;
	bool m_pointerOnUI;
	bool m_lastFrameStereo; // The last frame showed the game world
	float m_triggerHoldTime;
	Vec2s m_clickPosition;
	bool m_clickOnTarget;
	bool m_clickOnUI;
	bool m_clickByTrigger; // The trigger is used as the click (started on a panel)
	bool m_clickWasPressed;
	bool m_gripIsClick; // The grip press is a game click (not grabbing directly, see VRGrab)
	bool m_clickUseOnTap; // The trigger click started on a panel where a tap uses the item
	int m_useTapFrames; // Frames left of a use action from a trigger tap (press, then release in place)
	float m_useTapDelay; // s until a pending tap uses its item (waiting for a second tap), negative if none
	Vec2s m_useTapPosition; // Where the pending tap was
	bool m_secondTap; // The current trigger click is the second tap of a double tap (selects the item to combine)
	bool m_triggerOnItem; // The trigger was pressed on an item in the world (a tap, not magic)
	bool m_magicDrawing; // The trigger press may draw a rune or tap to toggle the magic mode (see updatePointer)
	bool m_magicMode; // Toggled by a tap of the trigger in the world: the game's magic mode is held
	bool m_bookOpen; // The book is open, held in a hand
	VRHand m_bookHand;
	float m_bookPixelSize; // m per interface pixel of the book panel
	Vec2f m_bookCoverSize; // m
	/*!
	 * An interface panel drawn in the world, so that the hands are in front of it (a quad layer would cover them): a
	 * copy of its part of the interface, and where it is relative to the wrist it is attached to
	 */
	struct WorldPanel {
		Texture * texture = nullptr;
		Vec2i textureSize = Vec2i(0);
		Vec2f size = Vec2f(0.f); // m
		bool active = false;
		VRHand hand = VRHandLeft;
		Vec3f localPosition = Vec3f(0.f);
		glm::quat localOrientation = glm::quat(1.f, 0.f, 0.f, 0.f);
	};
	WorldPanel m_bookPanel; // With the bookmarks
	WorldPanel m_inventoryPanel;
	//! Copy a part of the interface into the texture of a panel drawn in the world
	bool copyToWorldPanel(WorldPanel & panel, const Rect & area, float pixelSize);
	//! The model wrist joint and the controller orientation it follows, in the tracking space (else the controller)
	bool getWristFrame(VRHand hand, Vec3f & joint, glm::quat & grip) const;
	bool m_magicStroke; // The trigger press draws a rune (moved or held long enough, not a tap)
	bool m_magicPrecast; // The grip was held while drawing a rune in this magic mode: the spell is stored
	float m_magicCancelTime; // s left for a second tap ending the latest lasting spell, negative if none
	float m_magicPressTime; // s the trigger is held
	Vec3f m_magicPressPosition; // Where the fingertips were when the trigger was pressed, in the tracking space
	EntityHandle m_triggerItem;
	float m_triggerItemTime;
	EntityHandle m_lastItemTap; // The item of the last tap in the world, for double taps
	float m_lastItemTapAge;

	XrFrameState m_frameState;
	XrTime m_displayTime = 0; //!< When to locate the head and the controllers (the prediction, unless it is invalid)
	bool m_displayTimeFixed = false; //!< The prediction is invalid (seen after the headset reconnects)
	bool m_pauseRequested = false; //!< See consumePauseRequest()
	bool m_present = false; //!< The headset and a controller are tracked (see updatePresence)
	XrTime m_headsetAwaySince = 0;
	XrTime m_controllersAwaySince = 0;
	bool m_frameActive;
	bool m_shouldRender;
	bool m_rendered;

	std::string m_runtimeName;
	std::string m_systemName;

};

//! Non-null while VR is enabled
extern VRSystem * g_vr;

#endif // ARX_VR_VRSYSTEM_H
