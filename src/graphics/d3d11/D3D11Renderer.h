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

#ifndef ARX_GRAPHICS_D3D11_D3D11RENDERER_H
#define ARX_GRAPHICS_D3D11_D3D11RENDERER_H

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "graphics/d3d11/D3D11ComPtr.h"

#include <boost/intrusive/list.hpp>

#include "graphics/Renderer.h"
#include "graphics/d3d11/D3D11Texture.h"
#include "math/Rectangle.h"
#include "platform/Platform.h"

class D3D11TextureStage;
class D3D11Upscaler;

/*!
 * Direct3D 11 renderer backend.
 *
 * Emulates the fixed-function pipeline used by the OpenGL backend with a small set
 * of shaders. All rendering goes to an offscreen color + depth target that is
 * resolved / copied to the swap chain back buffer in \ref present().
 */
class D3D11Renderer final : public Renderer {

public:
	
	template <class T>
	using ComPtr = d3d11::ComPtr<T>;
	
	enum VertexType {
		VertexTL,
		VertexWorld,
		VertexWorld3,
		VertexTypeCount
	};
	
	D3D11Renderer();
	~D3D11Renderer() override;
	
	/*!
	 * Create the device and swap chain for the given window.
	 * Must be called before \ref initialize().
	 */
	bool create(HWND window, int maxMSAALevel);
	
	void initialize() override;
	
	void beforeResize(bool wasOrIsFullscreen) override;
	void afterResize() override;
	
	// Matrices
	void SetViewMatrix(const glm::mat4x4 & matView) override;
	void SetProjectionMatrix(const glm::mat4x4 & matProj) override;
	
	// Texture management
	void ReleaseAllTextures() override;
	void RestoreAllTextures() override;
	void reloadColorKeyTextures() override;
	
	// Factory
	Texture * createTexture() override;
	
	// Viewport
	void SetViewport(const Rect & viewport) override;
	
	void SetScissor(const Rect & rect) override;
	
	// Render Target
	void Clear(BufferFlags bufferFlags, Color clearColor = Color(), float clearDepth = 1.f,
	           size_t nrects = 0, Rect * rect = 0) override;
	
	// Fog
	void SetFogColor(Color color) override;
	void SetFogParams(float fogStart, float fogEnd) override;
	
	// Rasterizer
	void SetAntialiasing(bool enable) override;
	void SetFillMode(FillMode mode) override;
	
	float getMaxAnisotropy() const { return m_maximumAnisotropy; }
	float getMaxSupportedAnisotropy() const override { return m_maximumSupportedAnisotropy; }
	void setMaxAnisotropy(float value) override;
	
	AlphaCutoutAntialising getMaxSupportedAlphaCutoutAntialiasing() const override;
	
	std::unique_ptr<VertexBuffer<TexturedVertex>> createVertexBufferTL(size_t capacity, BufferUsage usage) override;
	std::unique_ptr<VertexBuffer<SMY_VERTEX>> createVertexBuffer(size_t capacity, BufferUsage usage) override;
	std::unique_ptr<VertexBuffer<SMY_VERTEX3>> createVertexBuffer3(size_t capacity, BufferUsage usage) override;
	
	void drawIndexed(Primitive primitive, const TexturedVertex * vertices, size_t nvertices,
	                 unsigned short * indices, size_t nindices) override;
	
	bool getSnapshot(Image & image) override;
	bool getSnapshot(Image & image, size_t width, size_t height) override;
	
	bool supportsSceneScale() const override { return true; }
	void setSceneScale(float scale) override;
	void beginScene() override;
	void endScene() override;
	bool supportsUpscaler(std::string_view name) const override;
	std::string getUpscalerError(std::string_view name) const override;
	void setUpscaler(std::string_view name, std::string_view preset) override;
	
	D3D11TextureStage * GetTextureStage(size_t textureStage) {
		return reinterpret_cast<D3D11TextureStage *>(Renderer::GetTextureStage(textureStage));
	}
	
	UINT msaaSamples() const { return m_msaaSamples; }
	
	void setVSync(int vsync) { m_vsync = vsync; }
	void present();
	
	ID3D11Device * device() const { return m_device.Get(); }
	ID3D11DeviceContext * context() const { return m_context.Get(); }
	
	//! Draw count vertices starting at offset from a vertex buffer.
	void drawBuffer(VertexType type, Primitive primitive, ID3D11Buffer * buffer, UINT stride,
	                size_t count, size_t offset);
	
	//! Draw indexed vertices from a vertex buffer, indices are relative to baseVertex.
	void drawBufferIndexed(VertexType type, Primitive primitive, ID3D11Buffer * buffer, UINT stride,
	                       size_t baseVertex, const unsigned short * indices, size_t nindices);
	
	//! Remove all references to a texture that is being destroyed.
	void onTextureDestroyed(D3D11Texture * texture);
	
	//! Unbind a texture view that is about to be released.
	void unbindTextureView(ID3D11ShaderResourceView * view);
	
	//! Mipmap LOD bias to add while rendering the 3D scene at a lower resolution
	float sceneLodBias() const { return m_inScene ? m_sceneLodBias : 0.f; }
	
private:
	
	struct RingBuffer {
		
		ComPtr<ID3D11Buffer> buffer;
		size_t size = 0;
		size_t pos = 0;
		
		//! Upload data and return the byte offset, or size_t(-1) on failure.
		size_t upload(ID3D11DeviceContext * context, const void * data, size_t bytes, size_t alignment);
		
	};
	
	bool createSizeDependentResources();
	bool createSceneTargets(const Vec2i & size);
	void releaseSceneTargets();
	
	//! \return true if the 3D scene is rendered to separate targets
	bool sceneEnabled() const;
	
	//! \return true if motion vectors and depth are needed by the upscaler or a debug view
	bool upscalerInputsNeeded() const;
	
	//! Track the scene camera and detect camera cuts
	void updateSceneCamera();
	
	//! Generate motion vectors and normalized depth for the scene
	void renderSceneMotion();
	
	//! Create the requested upscaler if that was not tried yet
	void updateUpscaler();
	
	//! Run the upscaler on the scene, returns the upscaled image or nullptr
	ID3D11ShaderResourceView * runUpscaler();
	
	//! Draw a full-screen triangle with the given pixel shader
	void drawFullscreen(ID3D11PixelShader * shader);
	
	//! Render target size divided by the window size
	Vec2f targetScale() const;
	D3D11_VIEWPORT toTargetViewport(const Rect & rect) const;
	D3D11_RECT toTargetRect(const Rect & rect) const;
	void bindRenderTargets();
	void releaseSizeDependentResources();
	bool compileShaders();
	
	void reinit();
	void shutdown();
	
	void flushState(VertexType type);
	void bindVertexBuffer(VertexType type, ID3D11Buffer * buffer, UINT stride, UINT offset);
	void setPrimitive(Primitive primitive);
	bool drawFan(size_t count, size_t baseVertex);
	bool uploadIndices(const unsigned short * indices, size_t nindices, Primitive primitive, UINT & start,
	                   size_t & count);
	void invalidateCachedState();
	
	ID3D11RasterizerState * getRasterizerState(bool cull, unsigned depthOffset, bool wireframe, bool scissor);
	ID3D11BlendState * getBlendState(BlendingFactor src, BlendingFactor dst, bool alphaToCoverage,
	                                 bool colorWrite);
	ID3D11DepthStencilState * getDepthState(bool test, bool write);
	
	HWND m_window;
	
	ComPtr<ID3D11Device> m_device;
	ComPtr<ID3D11DeviceContext> m_context;
	ComPtr<IDXGISwapChain1> m_swapChain;
	D3D_FEATURE_LEVEL m_featureLevel;
	bool m_flipModel;
	bool m_allowTearing;
	int m_vsync;
	
	Vec2i m_size;
	UINT m_msaaSamples;
	ComPtr<ID3D11Texture2D> m_colorTarget;
	ComPtr<ID3D11RenderTargetView> m_colorView;
	ComPtr<ID3D11Texture2D> m_depthTarget;
	ComPtr<ID3D11DepthStencilView> m_depthView;
	
	// 3D scene rendered at a lower resolution
	float m_sceneScale;
	bool m_inScene;
	Vec2i m_sceneSize;
	ComPtr<ID3D11Texture2D> m_sceneColorTarget;
	ComPtr<ID3D11RenderTargetView> m_sceneColorView;
	ComPtr<ID3D11Texture2D> m_sceneDepthTarget;
	ComPtr<ID3D11DepthStencilView> m_sceneDepthView;
	ComPtr<ID3D11Texture2D> m_sceneResolved;
	ComPtr<ID3D11ShaderResourceView> m_sceneResolvedView;
	ComPtr<ID3D11ShaderResourceView> m_sceneDepthResource;
	UINT m_sceneSamples;
	float m_sceneLodBias;
	
	// Upscaler inputs: motion vectors in pixels (current to previous frame) and depth in [0, 1]
	ComPtr<ID3D11Texture2D> m_motionTarget;
	ComPtr<ID3D11RenderTargetView> m_motionView;
	ComPtr<ID3D11ShaderResourceView> m_motionResource;
	ComPtr<ID3D11Texture2D> m_linearDepthTarget;
	ComPtr<ID3D11RenderTargetView> m_linearDepthView;
	ComPtr<ID3D11ShaderResourceView> m_linearDepthResource;
	// Per-pixel motion of objects that provide their previous position, written while rendering the scene
	ComPtr<ID3D11Texture2D> m_objectMotionTarget;
	ComPtr<ID3D11RenderTargetView> m_objectMotionView;
	ComPtr<ID3D11ShaderResourceView> m_objectMotionResource;
	
	// Scene camera tracking
	glm::mat4x4 m_sceneProjection;
	glm::mat4x4 m_previousProjection;
	glm::mat4x4 m_sceneView;
	glm::mat4x4 m_previousView;
	bool m_hasPreviousScene;
	bool m_sceneRenderedLastFrame;
	bool m_sceneRenderedThisFrame;
	bool m_resetHistory;
	
	// Sub-pixel jitter for temporal upscalers, in pixels
	bool m_jitterEnabled;
	u32 m_jitterIndex;
	Vec2f m_jitter;
	
	int m_sceneDebugView;
	
	// Temporal upscaler
	std::string m_upscalerName;
	std::string m_upscalerPreset;
	bool m_upscalerCreated;
	bool m_dlssFailed; //!< nvngx_dlss.dll was found but DLSS could not be initialized
	std::unique_ptr<D3D11Upscaler> m_upscaler;
	ComPtr<ID3D11Texture2D> m_upscaledTarget;
	ComPtr<ID3D11ShaderResourceView> m_upscaledView;
	LONGLONG m_lastUpscaleTime;
	
	ComPtr<ID3D11VertexShader> m_vertexShaders[VertexTypeCount];
	ComPtr<ID3D11InputLayout> m_inputLayouts[VertexTypeCount];
	ComPtr<ID3D11PixelShader> m_pixelShader;
	ComPtr<ID3D11PixelShader> m_pixelShaderSampleShading;
	ComPtr<ID3D11VertexShader> m_clearVertexShader;
	ComPtr<ID3D11PixelShader> m_clearPixelShader;
	ComPtr<ID3D11VertexShader> m_blitVertexShader;
	ComPtr<ID3D11PixelShader> m_blitPixelShader;
	ComPtr<ID3D11SamplerState> m_blitSampler;
	ComPtr<ID3D11PixelShader> m_motionPixelShader;
	ComPtr<ID3D11PixelShader> m_motionPixelShaderMS;
	ComPtr<ID3D11PixelShader> m_debugDepthPixelShader;
	ComPtr<ID3D11PixelShader> m_debugMotionPixelShader;
	ComPtr<ID3D11PixelShader> m_debugObjectMotionPixelShader;
	ComPtr<ID3D11Buffer> m_motionConstants;
	ComPtr<ID3D11Buffer> m_vsConstants;
	ComPtr<ID3D11Buffer> m_psConstants;
	ComPtr<ID3D11Buffer> m_clearConstants;
	
	RingBuffer m_vertexRing;
	RingBuffer m_indexRing;
	std::vector<unsigned short> m_indexScratch;
	
	std::unordered_map<u32, ComPtr<ID3D11RasterizerState>> m_rasterizerStates;
	std::unordered_map<u32, ComPtr<ID3D11BlendState>> m_blendStates;
	std::unordered_map<u32, ComPtr<ID3D11DepthStencilState>> m_depthStates;
	
	typedef boost::intrusive::list<D3D11Texture, boost::intrusive::constant_time_size<false>> TextureList;
	TextureList m_textures;
	
	float m_maximumAnisotropy;
	float m_maximumSupportedAnisotropy;
	
	Rect m_viewport;
	Rect m_scissor;
	bool m_wireframe;
	bool m_hasMSAA;
	
	glm::mat4x4 m_projection;
	glm::mat4x4 m_view;
	
	Color4f m_fogColor;
	float m_fogStart;
	float m_fogEnd;
	
	// Cached device state
	bool m_stateValid;
	VertexType m_currentVertexType;
	ID3D11Buffer * m_currentVertexBuffer;
	UINT m_currentVertexOffset;
	D3D11_PRIMITIVE_TOPOLOGY m_currentTopology;
	ID3D11RasterizerState * m_currentRasterizer;
	ID3D11BlendState * m_currentBlend;
	ID3D11DepthStencilState * m_currentDepth;
	ID3D11PixelShader * m_currentPixelShader;
	ID3D11ShaderResourceView * m_currentViews[3];
	ID3D11SamplerState * m_currentSamplers[3];
	float m_vsConstantsData[20];
	bool m_vsConstantsValid;
	unsigned char m_psConstantsData[96];
	bool m_psConstantsValid;
	
	friend class D3D11TextureStage;
	
};

#endif // ARX_GRAPHICS_D3D11_D3D11RENDERER_H
