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

#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif

#include "vr/VRGraphics.h"
#include "vr/VRFunctions.h"

#include <algorithm>
#include <iterator>

#include "core/Config.h"
#include "graphics/Renderer.h"
#include "graphics/opengl/GLTexture.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "graphics/opengl/OpenGLUtil.h"
#include "io/log/Logger.h"
#include <epoxy/wgl.h>
#include <unknwn.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr_platform.h>

namespace {

//! OpenXR with the OpenGL renderer: the eyes are rendered into a multisampled framebuffer and blitted
class VRGraphicsOpenGL final : public VRGraphics {

public:
	
	const char * getName() const override { return "OpenGL"; }
	const char * getExtension() const override { return XR_KHR_OPENGL_ENABLE_EXTENSION_NAME; }
	bool isDirect3D11() const override { return false; }
	
	bool checkRequirements(XrInstance instance, XrSystemId system) override;
	const void * getSessionBinding(XrInstance instance, XrSystemId system) override;
	
	s64 selectColorFormat(const std::vector<s64> & formats) const override;
	bool enumerateImages(XrSwapchain swapchain, std::vector<u64> & images) override;
	// The Meta runtime counts image rectangles of OpenGL swapchains from the top, SteamVR from the bottom
	bool areImageRectsFromTop(bool metaRuntime) const override { return metaRuntime; }
	bool isPanelCopyFlipped() const override { return true; }
	
	bool createStereoTarget(const Vec2i & eyeSize) override;
	void destroyStereoTarget() override;
	bool beginStereo(const std::vector<StereoView> & views) override;
	bool endStereo(bool uiActive) override;
	void copyEyeToImage(size_t eye, u64 image) override;
	void drawEyeToWindow(const Vec2i & windowSize) override;
	
	bool createUITarget(const Vec2i & size) override;
	void destroyUITarget() override;
	bool hasUITarget() const override { return m_uiColor != 0; }
	void beginUI() override;
	void endUI() override;
	void setPremultipliedAlphaOutput(bool enable) override;
	void copyUIToImage(u64 image) override;
	bool copyUIToTexture(Texture * texture, const Rect & area) override;
	void copyWindowToImage(u64 image, const Vec2i & windowSize) override;
	void drawUIToWindow(const Vec2i & windowSize, bool clear, const std::vector<VRWindowQuad> & quads) override;

private:
	
	//! Copy the read framebuffer into a texture
	void blitToImage(GLuint image, const Vec2i & sourceOffset, const Vec2i & sourceSize, const Vec2i & size);
	
	XrGraphicsBindingOpenGLWin32KHR m_binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
	
	Vec2i m_eyeSize = Vec2i(0);
	GLuint m_blitFramebuffer = 0;
	
	// Double-wide (left eye | right eye) multisampled render target and its resolve target
	GLuint m_stereoFramebuffer = 0;
	GLuint m_stereoColor = 0;
	GLuint m_stereoDepth = 0;
	GLuint m_stereoResolveFramebuffer = 0;
	GLuint m_stereoResolveColor = 0;
	GLuint m_stereoPreviousTarget = 0;
	Vec2i m_stereoPreviousTargetSize = Vec2i(0);
	
	GLuint m_uiFramebuffer = 0;
	GLuint m_uiColor = 0;
	GLuint m_uiDepth = 0;
	Vec2i m_uiSize = Vec2i(0);
	
};

OpenGLRenderer * getOpenGLRenderer() {
	return static_cast<OpenGLRenderer *>(GRenderer);
}

bool VRGraphicsOpenGL::checkRequirements(XrInstance instance, XrSystemId system) {
	ARX_UNUSED(instance);
	ARX_UNUSED(system);
	// Checked with the OpenGL context in getSessionBinding()
	return true;
}

const void * VRGraphicsOpenGL::getSessionBinding(XrInstance instance, XrSystemId system) {
	
	// Must be called before xrCreateSession
	PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nullptr;
	if(!checkXrResult(instance, xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR",
	                                                  reinterpret_cast<PFN_xrVoidFunction *>(&getRequirements)),
	                  "xrGetInstanceProcAddr(xrGetOpenGLGraphicsRequirementsKHR)")) {
		return nullptr;
	}
	XrGraphicsRequirementsOpenGLKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
	if(!checkXrResult(instance, getRequirements(instance, system, &requirements),
	                  "xrGetOpenGLGraphicsRequirementsKHR")) {
		return nullptr;
	}
	
	XrVersion glVersion = XR_MAKE_VERSION(epoxy_gl_version() / 10, epoxy_gl_version() % 10, 0);
	if(glVersion < requirements.minApiVersionSupported) {
		LogError << "OpenXR: runtime requires OpenGL " << XR_VERSION_MAJOR(requirements.minApiVersionSupported)
		         << '.' << XR_VERSION_MINOR(requirements.minApiVersionSupported);
		return nullptr;
	}
	
	if(!epoxy_has_gl_extension("GL_ARB_framebuffer_object") && epoxy_gl_version() < 30) {
		LogError << "OpenXR: OpenGL framebuffer objects are required";
		return nullptr;
	}
	
	m_binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
	m_binding.hDC = wglGetCurrentDC();
	m_binding.hGLRC = wglGetCurrentContext();
	if(!m_binding.hDC || !m_binding.hGLRC) {
		LogError << "OpenXR: no current OpenGL context";
		return nullptr;
	}
	
	return &m_binding;
}

s64 VRGraphicsOpenGL::selectColorFormat(const std::vector<s64> & formats) const {
	
	/*
	 * The game renders gamma-encoded colors. Storing them unchanged in an sRGB image (with GL_FRAMEBUFFER_SRGB
	 * disabled) makes the compositor decode them correctly.
	 */
	const s64 preferred[] = { GL_SRGB8_ALPHA8, GL_SRGB8, GL_RGBA8 };
	for(s64 format : preferred) {
		if(std::find(formats.begin(), formats.end(), format) != formats.end()) {
			return format;
		}
	}
	
	return 0;
}

bool VRGraphicsOpenGL::enumerateImages(XrSwapchain swapchain, std::vector<u64> & images) {
	
	uint32_t imageCount = 0;
	xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr);
	std::vector<XrSwapchainImageOpenGLKHR> glImages(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
	if(XR_FAILED(xrEnumerateSwapchainImages(swapchain, imageCount, &imageCount,
	                                        reinterpret_cast<XrSwapchainImageBaseHeader *>(glImages.data())))) {
		LogError << "OpenXR: xrEnumerateSwapchainImages failed";
		return false;
	}
	
	images.clear();
	for(const XrSwapchainImageOpenGLKHR & image : glImages) {
		images.push_back(image.image);
	}
	
	return true;
}

bool VRGraphicsOpenGL::createStereoTarget(const Vec2i & eyeSize) {
	
	m_eyeSize = eyeSize;
	const Vec2i size(m_eyeSize.x * 2, m_eyeSize.y);
	
	glGenFramebuffers(1, &m_blitFramebuffer);
	
	GLint maxSamples = 0;
	glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
	GLsizei samples = config.video.antialiasing ? std::min(GLint(config.vr.msaaSamples), maxSamples) : 0;
	
	glGenRenderbuffers(1, &m_stereoColor);
	glBindRenderbuffer(GL_RENDERBUFFER, m_stereoColor);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, size.x, size.y);
	
	glGenRenderbuffers(1, &m_stereoDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, m_stereoDepth);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, size.x, size.y);
	
	glGenRenderbuffers(1, &m_stereoResolveColor);
	glBindRenderbuffer(GL_RENDERBUFFER, m_stereoResolveColor);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, size.x, size.y);
	
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	
	glGenFramebuffers(1, &m_stereoFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_stereoFramebuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_stereoColor);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_stereoDepth);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	
	glGenFramebuffers(1, &m_stereoResolveFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_stereoResolveFramebuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_stereoResolveColor);
	GLenum resolveStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	
	if(status != GL_FRAMEBUFFER_COMPLETE || resolveStatus != GL_FRAMEBUFFER_COMPLETE) {
		LogError << "OpenXR: could not create the stereo render target (status 0x" << std::hex << status
		         << ", 0x" << resolveStatus << ')';
		destroyStereoTarget();
		return false;
	}
	
	LogInfo << "VR stereo render target: " << size.x << 'x' << size.y << ", " << samples << "x MSAA";
	
	return true;
}

void VRGraphicsOpenGL::destroyStereoTarget() {
	
	if(m_blitFramebuffer) {
		glDeleteFramebuffers(1, &m_blitFramebuffer);
		m_blitFramebuffer = 0;
	}
	
	GLuint framebuffers[] = { m_stereoFramebuffer, m_stereoResolveFramebuffer };
	glDeleteFramebuffers(GLsizei(std::size(framebuffers)), framebuffers);
	GLuint renderbuffers[] = { m_stereoColor, m_stereoDepth, m_stereoResolveColor };
	glDeleteRenderbuffers(GLsizei(std::size(renderbuffers)), renderbuffers);
	
	m_stereoFramebuffer = m_stereoResolveFramebuffer = 0;
	m_stereoColor = m_stereoDepth = m_stereoResolveColor = 0;
	
}

bool VRGraphicsOpenGL::beginStereo(const std::vector<StereoView> & views) {
	
	if(!m_stereoFramebuffer) {
		return false;
	}
	
	OpenGLRenderer * renderer = getOpenGLRenderer();
	m_stereoPreviousTarget = renderer->getRenderTarget();
	m_stereoPreviousTargetSize = renderer->getRenderTargetSize();
	renderer->setPremultipliedAlphaOutput(false);
	renderer->setRenderTarget(m_stereoFramebuffer, Vec2i(m_eyeSize.x * 2, m_eyeSize.y));
	renderer->setStereoViews(views);
	
	return true;
}

bool VRGraphicsOpenGL::endStereo(bool uiActive) {
	
	OpenGLRenderer * renderer = getOpenGLRenderer();
	renderer->setStereoViews({ });
	renderer->setRenderTarget(m_stereoPreviousTarget, m_stereoPreviousTargetSize);
	renderer->setPremultipliedAlphaOutput(uiActive);
	
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_SCISSOR_TEST);
	
	// Resolve multisampling - this cannot be combined with partial or scaled blits
	const Vec2i size(m_eyeSize.x * 2, m_eyeSize.y);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_stereoFramebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_stereoResolveFramebuffer);
	glBlitFramebuffer(0, 0, size.x, size.y, 0, 0, size.x, size.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	
	glBindFramebuffer(GL_FRAMEBUFFER, m_stereoPreviousTarget);
	if(scissor) {
		glEnable(GL_SCISSOR_TEST);
	}
	
	return true;
}

void VRGraphicsOpenGL::blitToImage(GLuint image, const Vec2i & sourceOffset, const Vec2i & sourceSize,
                                   const Vec2i & size) {
	
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_SCISSOR_TEST);
	
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_blitFramebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image, 0);
	glBlitFramebuffer(sourceOffset.x, sourceOffset.y, sourceOffset.x + sourceSize.x, sourceOffset.y + sourceSize.y,
	                  0, 0, size.x, size.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	
	if(scissor) {
		glEnable(GL_SCISSOR_TEST);
	}
	
}

void VRGraphicsOpenGL::copyEyeToImage(size_t eye, u64 image) {
	
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_stereoResolveFramebuffer);
	blitToImage(GLuint(image), Vec2i(s32(eye) * m_eyeSize.x, 0), m_eyeSize, m_eyeSize);
	glBindFramebuffer(GL_FRAMEBUFFER, getOpenGLRenderer()->getRenderTarget());
	
}

void VRGraphicsOpenGL::drawEyeToWindow(const Vec2i & windowSize) {
	
	Vec2i crop = m_eyeSize;
	if(m_eyeSize.x * windowSize.y > windowSize.x * m_eyeSize.y) {
		crop.x = m_eyeSize.y * windowSize.x / windowSize.y;
	} else {
		crop.y = m_eyeSize.x * windowSize.y / windowSize.x;
	}
	Vec2i offset = (m_eyeSize - crop) / s32(2);
	
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_SCISSOR_TEST);
	
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_stereoResolveFramebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBlitFramebuffer(offset.x, offset.y, offset.x + crop.x, offset.y + crop.y,
	                  0, 0, windowSize.x, windowSize.y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	
	glBindFramebuffer(GL_FRAMEBUFFER, getOpenGLRenderer()->getRenderTarget());
	if(scissor) {
		glEnable(GL_SCISSOR_TEST);
	}
	
}

bool VRGraphicsOpenGL::createUITarget(const Vec2i & size) {
	
	destroyUITarget();
	
	GLint previousFramebuffer = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
	GLint previousTexture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	
	glGenTextures(1, &m_uiColor);
	glBindTexture(GL_TEXTURE_2D, m_uiColor);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, GLuint(previousTexture));
	
	// The interface draws the 3D player model in the book, so it needs a depth buffer
	glGenRenderbuffers(1, &m_uiDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, m_uiDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size.x, size.y);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	
	glGenFramebuffers(1, &m_uiFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_uiFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_uiColor, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_uiDepth);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFramebuffer));
	
	if(status != GL_FRAMEBUFFER_COMPLETE) {
		LogError << "OpenXR: could not create the interface render target (status 0x" << std::hex << status << ')';
		destroyUITarget();
		return false;
	}
	
	m_uiSize = size;
	
	return true;
}

void VRGraphicsOpenGL::destroyUITarget() {
	
	if(m_uiFramebuffer) {
		glDeleteFramebuffers(1, &m_uiFramebuffer);
	}
	if(m_uiDepth) {
		glDeleteRenderbuffers(1, &m_uiDepth);
	}
	if(m_uiColor) {
		glDeleteTextures(1, &m_uiColor);
	}
	m_uiFramebuffer = m_uiDepth = m_uiColor = 0;
	m_uiSize = Vec2i(0);
	
}

void VRGraphicsOpenGL::beginUI() {
	
	OpenGLRenderer * renderer = getOpenGLRenderer();
	renderer->setRenderTarget(m_uiFramebuffer, m_uiSize);
	renderer->setPremultipliedAlphaOutput(true);
	renderer->Clear(Renderer::ColorBuffer | Renderer::DepthBuffer, Color(0, 0, 0, 0));
	
}

void VRGraphicsOpenGL::endUI() {
	
	OpenGLRenderer * renderer = getOpenGLRenderer();
	renderer->setPremultipliedAlphaOutput(false);
	renderer->setRenderTarget(0, Vec2i(0));
	
}

void VRGraphicsOpenGL::setPremultipliedAlphaOutput(bool enable) {
	getOpenGLRenderer()->setPremultipliedAlphaOutput(enable);
}

void VRGraphicsOpenGL::copyUIToImage(u64 image) {
	
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_uiFramebuffer);
	blitToImage(GLuint(image), Vec2i(0), m_uiSize, m_uiSize);
	glBindFramebuffer(GL_FRAMEBUFFER, getOpenGLRenderer()->getRenderTarget());
	
}

bool VRGraphicsOpenGL::copyUIToTexture(Texture * texture, const Rect & area) {
	
	if(!m_uiColor || !texture) {
		return false;
	}
	
	// OpenGL images start at the bottom: the copy is upside down
	GLuint name = static_cast<GLTexture *>(texture)->getGLTexture();
	glCopyImageSubData(m_uiColor, GL_TEXTURE_2D, 0, area.left, m_uiSize.y - area.bottom, 0,
	                   name, GL_TEXTURE_2D, 0, 0, 0, 0, area.width(), area.height(), 1);
	
	return true;
}

void VRGraphicsOpenGL::copyWindowToImage(u64 image, const Vec2i & windowSize) {
	
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	blitToImage(GLuint(image), Vec2i(0), windowSize, m_uiSize);
	glBindFramebuffer(GL_FRAMEBUFFER, getOpenGLRenderer()->getRenderTarget());
	
}

void VRGraphicsOpenGL::drawUIToWindow(const Vec2i & windowSize, bool clear, const std::vector<VRWindowQuad> & quads) {
	
	// Draw with the fixed-function pipeline, saving and restoring all state the renderer caches
	GLint previousFramebuffer = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
	GLint previousActiveTexture = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
	GLint textureUnits = 1;
	glGetIntegerv(GL_MAX_TEXTURE_UNITS, &textureUnits);
	
	glPushAttrib(GL_ALL_ATTRIB_BITS);
	
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, windowSize.x, windowSize.y);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_FOG);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	
	if(clear) {
		glClearColor(0.f, 0.f, 0.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	
	std::vector<GLint> previousTextures(static_cast<size_t>(textureUnits), 0);
	for(GLint unit = textureUnits - 1; unit >= 0; unit--) {
		glActiveTexture(GLenum(GL_TEXTURE0 + unit));
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextures[size_t(unit)]);
		glDisable(GL_TEXTURE_2D);
	}
	
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, m_uiColor);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	
	// Premultiplied alpha
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	
	for(const VRWindowQuad & quad : quads) {
		glBegin(GL_TRIANGLE_STRIP);
		for(size_t i = 0; i < 4; i++) {
			// OpenGL images start at the bottom
			glTexCoord2f(quad.uv[i].x, 1.f - quad.uv[i].y);
			glVertex4f(quad.position[i].x, quad.position[i].y, quad.position[i].z, quad.position[i].w);
		}
		glEnd();
	}
	
	for(GLint unit = textureUnits - 1; unit >= 0; unit--) {
		glActiveTexture(GLenum(GL_TEXTURE0 + unit));
		glBindTexture(GL_TEXTURE_2D, GLuint(previousTextures[size_t(unit)]));
	}
	
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	
	glPopAttrib();
	
	glActiveTexture(GLenum(previousActiveTexture));
	glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFramebuffer));
	
}
	
} // anonymous namespace

std::unique_ptr<VRGraphics> createVRGraphicsOpenGL() {
	return std::make_unique<VRGraphicsOpenGL>();
}
