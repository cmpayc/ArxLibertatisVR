/*
 * Copyright 2011-2022 Arx Libertatis Team (see the AUTHORS file)
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

#ifndef ARX_GRAPHICS_RENDERER_H
#define ARX_GRAPHICS_RENDERER_H

#include <stddef.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "graphics/Color.h"
#include "graphics/texture/TextureStage.h"
#include "math/Types.h"
#include "platform/Platform.h"
#include "util/Flags.h"

struct TexturedVertex;
struct SMY_VERTEX;
struct SMY_VERTEX3;
class TextureContainer;
class Image;
class Texture;
template <class Vertex> class VertexBuffer;

enum BlendingFactor {
	BlendZero,              //!< Zero
	BlendOne,               //!< One
	BlendSrcColor,          //!< Source color
	BlendSrcAlpha,          //!< Source alpha
	BlendInvSrcColor,       //!< One minus source color
	BlendInvSrcAlpha,       //!< One minus source alpha
	BlendSrcAlphaSaturate,  //!< Source alpha saturate
	BlendDstColor,          //!< Destination color
	BlendDstAlpha,          //!< Destination alpha
	BlendInvDstColor,       //!< One minus destination color
	BlendInvDstAlpha        //!< One minus destination alpha
};

class RenderState {
	
	static inline constexpr size_t DepthOffsetSize = 5;
	static inline constexpr size_t BlendSize = 4;
	
	enum Offsets {
		Cull,
		Fog,
		AlphaCutout,
		DepthTest,
		DepthWrite,
		DepthOffset,
		BlendSrc = DepthOffset + DepthOffsetSize,
		BlendDst = BlendSrc + BlendSize,
		End = BlendDst + BlendSize
	};
	
	// We could use bitfields here instead but they are missing (an efficient) operator==.
	u32 m_state = 0;
	
	template <size_t Offset, size_t Size>
	[[nodiscard]] constexpr u32 get() const noexcept {
		return (m_state >> Offset) & ((u32(1) << Size) - 1);
	}
	
	template <size_t Offset, size_t Size>
	constexpr void set(u32 value) noexcept {
		m_state = (m_state & ~(((u32(1) << Size) - 1) << Offset)) | (value << Offset);
	}
	
public:
	
	constexpr RenderState() noexcept {
		static_assert(sizeof(m_state) * 8 >= End, "fields do not fit into m_state");
		disableBlend();
	}
	
	[[nodiscard]] constexpr bool operator==(const RenderState & o) const noexcept {
		return m_state == o.m_state;
	}
	[[nodiscard]] constexpr bool operator!=(const RenderState & o) const noexcept {
		return m_state != o.m_state;
	}
	
	constexpr void setCull(bool cullBackfaces) noexcept {
		set<Cull, 1>(cullBackfaces);
	}
	
	[[nodiscard]] constexpr RenderState cull(bool cullBackfaces = true) const noexcept {
		RenderState copy = *this;
		copy.setCull(cullBackfaces);
		return copy;
	}
	
	[[nodiscard]] constexpr bool getCull() const noexcept {
		return get<Cull, 1>() != 0;
	}
	
	constexpr void setFog(bool enable) noexcept {
		set<Fog, 1>(enable);
	}
	
	[[nodiscard]] constexpr RenderState fog(bool enable = true) const noexcept {
		RenderState copy = *this;
		copy.setFog(enable);
		return copy;
	}
	
	[[nodiscard]] constexpr bool getFog() const noexcept {
		return get<Fog, 1>() != 0;
	}
	
	constexpr void setAlphaCutout(bool enable) noexcept {
		set<AlphaCutout, 1>(enable);
	}
	
	[[nodiscard]] constexpr RenderState alphaCutout(bool enable = true) const noexcept {
		RenderState copy = *this;
		copy.setAlphaCutout(enable);
		return copy;
	}
	
	[[nodiscard]] constexpr bool getAlphaCutout() const noexcept {
		return get<AlphaCutout, 1>() != 0;
	}
	
	constexpr void setDepthTest(bool enable) noexcept {
		set<DepthTest, 1>(enable);
	}
	
	[[nodiscard]] constexpr RenderState depthTest(bool enable = true) const noexcept {
		RenderState copy = *this;
		copy.setDepthTest(enable);
		return copy;
	}
	
	[[nodiscard]] constexpr bool getDepthTest() const noexcept {
		return get<DepthTest, 1>() != 0;
	}
	
	constexpr void setDepthWrite(bool enable) noexcept {
		set<DepthWrite, 1>(enable);
	}
	
	[[nodiscard]] constexpr RenderState depthWrite(bool enable = true) const noexcept {
		RenderState copy = *this;
		copy.setDepthWrite(enable);
		return copy;
	}
	
	[[nodiscard]] constexpr bool getDepthWrite() const noexcept {
		return get<DepthWrite, 1>() != 0;
	}
	
	constexpr void setDepthOffset(unsigned offset) noexcept {
		arx_assert(offset < (1u << DepthOffsetSize));
		set<DepthOffset, DepthOffsetSize>(offset);
	}
	
	constexpr void disableDepthOffset() noexcept {
		setDepthOffset(0);
	}
	
	[[nodiscard]] constexpr RenderState depthOffset(unsigned offset) const noexcept {
		RenderState copy = *this;
		copy.setDepthOffset(offset);
		return copy;
	}
	
	[[nodiscard]] constexpr RenderState noDepthOffset() const noexcept {
		RenderState copy = *this;
		copy.disableDepthOffset();
		return copy;
	}
	
	[[nodiscard]] constexpr unsigned getDepthOffset() const noexcept {
		return get<DepthOffset, DepthOffsetSize>();
	}
	
	constexpr void setBlend(BlendingFactor src, BlendingFactor dst) noexcept {
		arx_assert(src >= 0 && unsigned(src) < (1u << BlendSize));
		arx_assert(dst >= 0 && unsigned(dst) < (1u << BlendSize));
		set<BlendSrc, BlendSize>(src);
		set<BlendDst, BlendSize>(dst);
	}
	
	constexpr void setBlendAdditive() noexcept {
		setBlend(BlendSrcAlpha, BlendOne);
	}
	
	constexpr void disableBlend() noexcept {
		setBlend(BlendOne, BlendZero);
	}
	
	[[nodiscard]] constexpr RenderState blend(BlendingFactor src = BlendSrcAlpha,
	                                          BlendingFactor dst = BlendInvSrcAlpha) const noexcept {
		RenderState copy = *this;
		copy.setBlend(src, dst);
		return copy;
	}
	
	[[nodiscard]] constexpr RenderState blendAdditive() const noexcept {
		RenderState copy = *this;
		copy.setBlendAdditive();
		return copy;
	}
	
	[[nodiscard]] constexpr RenderState noBlend() const noexcept {
		RenderState copy = *this;
		copy.disableBlend();
		return copy;
	}
	
	[[nodiscard]] constexpr BlendingFactor getBlendSrc() const noexcept {
		return BlendingFactor(get<BlendSrc, BlendSize>());
	}
	
	[[nodiscard]] constexpr BlendingFactor getBlendDst() const noexcept {
		return BlendingFactor(get<BlendDst, BlendSize>());
	}
	
	[[nodiscard]] constexpr bool isBlendEnabled() const noexcept {
		return getBlendSrc() != BlendOne || getBlendDst() != BlendZero;
	}
	
};

class Renderer {
	
public:
	
	class Listener {
		
	public:
		
		virtual ~Listener() = default;
		
		virtual void onRendererInit(Renderer & renderer) { ARX_UNUSED(renderer); }
		virtual void onRendererShutdown(Renderer & renderer) { ARX_UNUSED(renderer); }
		
	};
	
	//! Pixel comparison functions
	enum PixelCompareFunc {
		CmpNever,               //!< Never
		CmpLess,                //!< Less
		CmpEqual,               //!< Equal
		CmpLessEqual,           //!< Less Equal
		CmpGreater,             //!< Greater
		CmpNotEqual,            //!< Not Equal
		CmpGreaterEqual,        //!< Greater Equal
		CmpAlways               //!< Always
	};
	
	enum FillMode {
		FillWireframe,
		FillSolid
	};
	
	//! Target surface
	enum BufferType {
		ColorBuffer = 1 << 0,
		DepthBuffer = 1 << 1
	};
	DECLARE_FLAGS(BufferType, BufferFlags)
	
	enum Primitive {
		TriangleList,
		TriangleStrip,
		TriangleFan,
		LineList,
		LineStrip
	};
	
	enum BufferUsage {
		Static,
		Dynamic,
		Stream
	};
	
	enum AlphaCutoutAntialising {
		NoAlphaCutoutAA = 0,
		FuzzyAlphaCutoutAA = 1,
		CrispAlphaCutoutAA = 2
	};
	
	Renderer();
	virtual ~Renderer();
	
	/*!
	 * Basic renderer initialization.
	 * Renderer will not be fully initialized until calling \ref afterResize().
	 * Does *not* notify any listeners.
	 */
	virtual void initialize() = 0;
	
	//! * \return true if the renderer has been fully initialized and is ready for use.
	bool isInitialized() const { return m_initialized; }
	
	/*!
	 * Indicate that the renderer's window will be resized and the renderer may need
	 * to temporarily shutdown.
	 * Will notify listeners if the renderer has been shut down.
	 */
	virtual void beforeResize(bool wasOrIsFullscreen) = 0;
	
	/*!
	 * Indicate the renderer's window has been resized and the renderer may need to be
	 * (re-)initialized.
	 * Will notify listeners if the renderer wasn't already initialized.
	 */
	virtual void afterResize() = 0;
	
	void addListener(Listener * listener);
	void removeListener(Listener * listener);
	
	//! Move all listeners to another renderer, used when falling back to a different backend.
	void transferListeners(Renderer & renderer);
	
	// Matrices
	virtual void SetViewMatrix(const glm::mat4x4 & matView) = 0;
	virtual void SetProjectionMatrix(const glm::mat4x4 & matProj) = 0;
	
	// Texture management
	virtual void ReleaseAllTextures() = 0;
	virtual void RestoreAllTextures() = 0;
	virtual void reloadColorKeyTextures() = 0;

	// Factory
	[[nodiscard]] virtual Texture * createTexture() = 0;
	
	// Viewport
	virtual void SetViewport(const Rect & viewport) = 0;
	
	// Scissor
	virtual void SetScissor(const Rect & rect) = 0;
	
	// Render Target
	virtual void Clear(BufferFlags bufferFlags, Color clearColor = Color(), float clearDepth = 1.f,
	                   size_t nrects = 0, Rect * rect = 0) = 0;
	
	// Fog
	virtual void SetFogColor(Color color) = 0;
	virtual void SetFogParams(float fogStart, float fogEnd) = 0;
	
	// Rasterizer
	virtual void SetAntialiasing(bool enable) = 0;
	virtual void SetFillMode(FillMode mode) = 0;
	
	// Texturing
	[[nodiscard]] size_t getTextureStageCount() const { return m_TextureStages.size(); }
	[[nodiscard]] TextureStage * GetTextureStage(size_t textureStage);
	[[nodiscard]] const TextureStage * GetTextureStage(size_t textureStage) const;
	void ResetTexture(unsigned int textureStage);
	[[nodiscard]] Texture * GetTexture(unsigned int textureStage) const;
	void SetTexture(unsigned int textureStage, Texture * pTexture);
	void SetTexture(unsigned int textureStage, TextureContainer * pTextureContainer);
	
	[[nodiscard]] virtual float getMaxSupportedAnisotropy() const = 0;
	virtual void setMaxAnisotropy(float value) = 0;
	
	[[nodiscard]] virtual AlphaCutoutAntialising getMaxSupportedAlphaCutoutAntialiasing() const = 0;
	
	[[nodiscard]] virtual std::unique_ptr<VertexBuffer<TexturedVertex>> createVertexBufferTL(size_t capacity, BufferUsage usage) = 0;
	[[nodiscard]] virtual std::unique_ptr<VertexBuffer<SMY_VERTEX>> createVertexBuffer(size_t capacity, BufferUsage usage) = 0;
	[[nodiscard]] virtual std::unique_ptr<VertexBuffer<SMY_VERTEX3>> createVertexBuffer3(size_t capacity, BufferUsage usage) = 0;
	
	virtual void drawIndexed(Primitive primitive, const TexturedVertex * vertices, size_t nvertices, unsigned short * indices, size_t nindices) = 0;
	
	virtual bool getSnapshot(Image & image) = 0;
	virtual bool getSnapshot(Image & image, size_t width, size_t height) = 0;
	
	// Scene rendering
	
	//! \return true if the 3D scene can be rendered at a lower resolution than the window
	[[nodiscard]] virtual bool supportsSceneScale() const { return false; }
	
	//! Set the fraction of the window resolution used for the 3D scene
	virtual void setSceneScale(float scale) { ARX_UNUSED(scale); }
	
	/*!
	 * Following draw calls render the 3D scene, which may use a lower resolution than the window.
	 * Coordinates are still specified in window pixels.
	 */
	virtual void beginScene() { }
	
	//! Upscale the 3D scene to the window, following draw calls render at the window resolution
	virtual void endScene() { }
	
	//! \return true if the named temporal upscaler (e.g. "dlss") is built into this renderer
	[[nodiscard]] virtual bool supportsUpscaler(std::string_view name) const { ARX_UNUSED(name); return false; }
	
	/*!
	 * Check if a supported upscaler can actually be used.
	 * \return an empty string if the upscaler is available, otherwise the reason as an identifier:
	 *         "dll_missing" if a required library was not found or "unsupported"
	 */
	[[nodiscard]] virtual std::string getUpscalerError(std::string_view name) const {
		ARX_UNUSED(name);
		return "unsupported";
	}
	
	/*!
	 * Select the temporal upscaler for the 3D scene.
	 * \param name "off" or an upscaler supported by \ref supportsUpscaler()
	 * \param preset upscaler-specific quality preset
	 */
	virtual void setUpscaler(std::string_view name, std::string_view preset) {
		ARX_UNUSED(name), ARX_UNUSED(preset);
	}
	
	//! Set the strength of the upscaler's sharpening pass in [0, 1], 0 disables it (only used by FSR)
	virtual void setUpscalerSharpness(float sharpness) { ARX_UNUSED(sharpness); }
	
	//! \return true if this renderer supports screen-space ambient occlusion for the 3D scene
	[[nodiscard]] virtual bool supportsAmbientOcclusion() const { return false; }
	
	//! Set the strength of screen-space ambient occlusion for the 3D scene in [0, 1], 0 disables it
	virtual void setAmbientOcclusion(float strength) { ARX_UNUSED(strength); }
	
	//! \return true if this renderer supports bloom for the 3D scene
	[[nodiscard]] virtual bool supportsBloom() const { return false; }
	
	//! Set the strength of the bloom for the 3D scene in [0, 1], 0 disables it
	virtual void setBloom(float strength) { ARX_UNUSED(strength); }
	
	//! \return true if this renderer supports color grading for the 3D scene
	[[nodiscard]] virtual bool supportsColorGrading() const { return false; }
	
	//! Select the color grading preset for the 3D scene: "off", "natural", "warm" or "cinematic"
	virtual void setColorGrading(std::string_view preset) { ARX_UNUSED(preset); }
	
	//! A light for per-pixel lighting, see ApplyTileLights()
	struct PixelLight {
		Vec3f position = Vec3f(0.f);
		float fallStart = 0.f;
		Color3f color = Color3f::black; //!< In [0, 1]
		float fallEnd = 0.f;
		float intensity = 0.f;          //!< Factor for the light color at full strength
		bool specularOnly = false;      //!< Only adds highlights, the diffuse light is already in the vertex colors
	};
	
	//! \return true if this renderer can light world geometry per pixel
	[[nodiscard]] virtual bool supportsPixelLighting() const { return false; }
	
	//! Enable or disable per-pixel lighting of world geometry, if supported
	virtual void setPixelLighting(bool enabled) { ARX_UNUSED(enabled); }
	
	/*!
	 * \return true if world geometry is lit per pixel with the lights from \ref setPixelLights().
	 *         The vertex colors of world geometry then only contain the static lighting.
	 */
	[[nodiscard]] virtual bool usesPixelLighting() const { return false; }
	
	//! \return true if this renderer supports normal maps for per-pixel lighting
	[[nodiscard]] virtual bool supportsNormalMaps() const { return false; }
	
	//! Set the strength of the normal maps generated from the textures in [0, 1], 0 disables them
	virtual void setNormalMaps(float strength) { ARX_UNUSED(strength); }
	
	//! \return true if this renderer supports parallax occlusion mapping
	[[nodiscard]] virtual bool supportsParallaxMapping() const { return false; }
	
	//! Set the depth of the parallax occlusion mapping of world geometry in [0, 1], 0 disables it
	virtual void setParallaxMapping(float strength) { ARX_UNUSED(strength); }
	
	//! \return true if this renderer supports specular highlights on world geometry
	[[nodiscard]] virtual bool supportsSpecular() const { return false; }
	
	//! Set the strength of the specular highlights of world geometry in [0, 1], 0 disables them
	virtual void setSpecular(float strength) { ARX_UNUSED(strength); }
	
	//! \return true if this renderer supports screen-space reflections on world geometry
	[[nodiscard]] virtual bool supportsReflections() const { return false; }
	
	//! Set the strength of the screen-space reflections of world geometry in [0, 1], 0 disables them
	virtual void setReflections(float strength) { ARX_UNUSED(strength); }
	
	//! Mark the following draws as horizontal water surfaces for highlights and reflections, until reset
	virtual void setWaterSurface(bool water) { ARX_UNUSED(water); }
	
	/*!
	 * \return true if the renderer currently turns model triangles into curved patches itself, using
	 *         TexturedVertex::tessellation and TexturedVertex::normal, see animation/MeshSmoothing.h
	 */
	[[nodiscard]] virtual bool supportsModelTessellation() const { return false; }
	
	//! Mark the following triangle lists as model batches that may be tessellated, until reset
	virtual void setModelTessellation(bool enabled) { ARX_UNUSED(enabled); }
	
	/*!
	 * Set the lights for per-pixel lighting of world vertices with a non-zero normal.
	 * Lights that only add highlights must come after all other lights.
	 */
	virtual void setPixelLights(const PixelLight * lights, size_t count) { ARX_UNUSED(lights), ARX_UNUSED(count); }

	//! A point light that casts dynamic shadows, with the same falloff as a \ref PixelLight
	struct ShadowLight {
		Vec3f position = Vec3f(0.f);
		float fallStart = 0.f;
		float fallEnd = 0.f;
		Color3f color = Color3f::black; //!< In [0, 1]
		float intensity = 0.f;          //!< Factor for the light color at full strength
		float strength = 0.f;           //!< In [0, 1], for fading the shadows in and out
		bool baked = false;             //!< The light is in the static lighting, which already has the world's shadows
	};

	//! \return true if this renderer supports dynamic shadows
	[[nodiscard]] virtual bool supportsShadows() const { return false; }

	//! Set the dynamic shadow quality: 0 = off (blob shadows), 1 to 3 = more lights and sharper shadows
	virtual void setShadows(int quality) { ARX_UNUSED(quality); }

	//! \return the maximum number of lights for \ref setShadowLights(), 0 if dynamic shadows are off
	[[nodiscard]] virtual size_t getShadowLightLimit() const { return 0; }

	//! Set the lights that cast dynamic shadows in the next 3D scene, at most \ref getShadowLightLimit()
	virtual void setShadowLights(const ShadowLight * lights, size_t count) { ARX_UNUSED(lights), ARX_UNUSED(count); }

	/*!
	 * Set the static world geometry that blocks the light of shadow casting lights.
	 * \param vertices world space triangle list, nullptr to remove the geometry of the previous level
	 */
	virtual void setShadowOccluders(const Vec3f * vertices, size_t count) { ARX_UNUSED(vertices), ARX_UNUSED(count); }

	/*!
	 * Add moving geometry that casts shadows in the current 3D scene.
	 * \param vertices world space triangle list, with the faces that the camera does not see
	 */
	virtual void addShadowCasters(const Vec3f * vertices, size_t count) { ARX_UNUSED(vertices), ARX_UNUSED(count); }

	void setRenderState(RenderState state) noexcept { m_state = state; }
	[[nodiscard]] RenderState getRenderState() const noexcept { return m_state; }
	
protected:
	
	std::vector<std::unique_ptr<TextureStage>> m_TextureStages;
	bool m_initialized;
	RenderState m_state;
	
	void onRendererInit();
	void onRendererShutdown();
	
private:
	
	typedef std::vector<Listener *> Listeners;
	
	Listeners m_listeners; //! Listeners for renderer events
	
};

DECLARE_FLAGS_OPERATORS(Renderer::BufferFlags)

extern Renderer * GRenderer;

/*!
 * RAII helper class to set a render state for the current scope
 *
 * Sets the requested render state on construction and restores the old render state
 * on destruction.
 *
 * Example usage:
 * \code
 * {
 *   UseRenderState state(RenderState().blendAdditive();
 *   // render with additive blending
 * }
 * \endcode
 */
class [[nodiscard]] UseRenderState {
	
	RenderState m_old;
	
public:
	
	explicit UseRenderState(RenderState state) noexcept
		: m_old(GRenderer->getRenderState())
	{
		GRenderer->setRenderState(state);
	}
	
	UseRenderState(const UseRenderState &) = delete;
	UseRenderState & operator=(const UseRenderState &) = delete;
	
	~UseRenderState() {
		GRenderer->setRenderState(m_old);
	}
	
};

/*!
 * RAII helper class to set a texture state for the current scope
 *
 * Sets the requested texture state on construction and restores the old texture state
 * on destruction.
 */
class [[nodiscard]] UseTextureState {
	
	TextureStage::WrapMode m_oldWrapMode;
	TextureStage::FilterMode m_oldMinFilter;
	TextureStage::FilterMode m_oldMagFilter;
	
public:
	
	UseTextureState(TextureStage::FilterMode minFilter, TextureStage::FilterMode magFilter,
	                TextureStage::WrapMode wrapMode)
		: m_oldWrapMode(GRenderer->GetTextureStage(0)->getWrapMode())
		, m_oldMinFilter(GRenderer->GetTextureStage(0)->getMinFilter())
		, m_oldMagFilter(GRenderer->GetTextureStage(0)->getMagFilter())
	{
		GRenderer->GetTextureStage(0)->setWrapMode(wrapMode);
		GRenderer->GetTextureStage(0)->setMinFilter(minFilter);
		GRenderer->GetTextureStage(0)->setMagFilter(magFilter);
	}
	
	UseTextureState(TextureStage::FilterMode filter, TextureStage::WrapMode wrapMode)
		: m_oldWrapMode(GRenderer->GetTextureStage(0)->getWrapMode())
		, m_oldMinFilter(GRenderer->GetTextureStage(0)->getMinFilter())
		, m_oldMagFilter(GRenderer->GetTextureStage(0)->getMagFilter())
	{
		GRenderer->GetTextureStage(0)->setWrapMode(wrapMode);
		GRenderer->GetTextureStage(0)->setMinFilter(filter);
		GRenderer->GetTextureStage(0)->setMagFilter(filter);
	}
	
	UseTextureState(const UseTextureState &) = delete;
	UseTextureState & operator=(const UseTextureState &) = delete;
	
	~UseTextureState() {
		GRenderer->GetTextureStage(0)->setWrapMode(m_oldWrapMode);
		GRenderer->GetTextureStage(0)->setMinFilter(m_oldMinFilter);
		GRenderer->GetTextureStage(0)->setMagFilter(m_oldMagFilter);
	}
	
};

//! Default render state for 2D compositing
[[nodiscard]] inline constexpr RenderState render2D() noexcept {
	return RenderState().blend();
}

//! Default render state for 3D rendering
[[nodiscard]] inline constexpr RenderState render3D() noexcept {
	return RenderState().depthTest().depthWrite().fog();
}

#endif // ARX_GRAPHICS_RENDERER_H
