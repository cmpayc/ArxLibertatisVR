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

#ifndef ARX_GRAPHICS_VULKAN_VULKANRENDERER_H
#define ARX_GRAPHICS_VULKAN_VULKANRENDERER_H

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <map>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <volk.h>
#include <vk_mem_alloc.h>

#include <boost/intrusive/list.hpp>

#include "graphics/Color.h"
#include "graphics/Renderer.h"
#include "graphics/StereoView.h"
#include "graphics/texture/TextureStage.h"
#include "graphics/vulkan/VulkanTexture.h"
#include "math/Rectangle.h"
#include "platform/Platform.h"

struct SDL_Window;
class VulkanTextureStage;
namespace vulkan { class ExternalDevice; }

/*!
 * Vulkan renderer backend.
 *
 * Like the Direct3D 11 backend, everything is rendered into an offscreen color + depth target (dynamic rendering)
 * that is resolved / copied into the swapchain image in \ref present().
 *
 * Resources still used by frames in flight are released with \ref deferRelease().
 */
class VulkanRenderer final : public Renderer {

public:

	enum VertexType {
		VertexTL,
		VertexWorld,
		VertexWorld3,
		VertexTypeCount
	};

	VulkanRenderer();
	~VulkanRenderer() override;

	bool create(SDL_Window * window, int maxMSAALevel);

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

	// Scissor
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

	// Texturing
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

	bool supportsPixelLighting() const override { return true; }
	void setPixelLighting(bool enabled) override;
	bool usesPixelLighting() const override { return m_pixelLighting; }
	void setPixelLights(const PixelLight * lights, size_t count) override;
	bool supportsNormalMaps() const override { return true; }
	void setNormalMaps(float strength) override;
	bool supportsParallaxMapping() const override { return true; }
	void setParallaxMapping(float strength) override;
	bool supportsSpecular() const override { return true; }
	void setSpecular(float strength) override;
	void setWaterSurface(bool water) override { m_waterSurface = water; }
	bool supportsBloom() const override { return true; }
	void setBloom(float strength) override;
	bool supportsColorGrading() const override { return true; }
	void setColorGrading(std::string_view preset) override;

	bool reliefMapsEnabled() const {
		return (m_normalMapStrength > 0.f || m_parallaxStrength > 0.f) && m_reliefPixelShader;
	}

	/*!
	 * Generate the relief map (brightness slope and height) of a texture with mipmaps, see psNormalMap
	 *
	 * Recorded with the uploads, after the uploads of the texture.
	 */
	void createReliefMap(VkImageView color, const Vec2i & size, VkImage & image, VmaAllocation & allocation,
	                     VkImageView & view);

	VulkanTextureStage * GetTextureStage(size_t textureStage) {
		return reinterpret_cast<VulkanTextureStage *>(Renderer::GetTextureStage(textureStage));
	}

	void setVSync(int vsync);
	void present();

	int msaaSamples() const { return int(m_msaaSamples); }

	//! The copy of a vertex buffer in the memory of the current frame
	struct FrameCopy {
		uint64_t frame = 0; //!< 0 if the buffer changed since the copy
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceSize offset = 0;
		size_t count = 0; //!< Copied vertices
	};

	/*!
	 * Draw vertices of a vertex buffer
	 *
	 * \param vertices The vertices of the buffer
	 * \param count The number of vertices used in the buffer
	 * \param copy nullptr to copy only the drawn vertices for this draw (stream buffers), or the copy of the whole
	 *             buffer which is made once per frame and reused for all draws until the buffer changes
	 */
	void drawBuffer(VertexType type, Primitive primitive, const void * vertices, size_t count, size_t first,
	                size_t drawCount, FrameCopy * copy);
	void drawBufferIndexed(VertexType type, Primitive primitive, const void * vertices, size_t count,
	                       size_t baseVertex, const unsigned short * indices, size_t nindices, FrameCopy * copy);

	void onTextureDestroyed(VulkanTexture * texture);

	VkDevice device() const { return m_device; }
	VmaAllocator allocator() const { return m_allocator; }

	/*!
	 * Command buffer for copies into textures, submitted before the commands of the current frame
	 *
	 * Copies are not allowed while rendering, and textures are also uploaded outside of frames (e.g. while loading).
	 */
	VkCommandBuffer beginUpload();

	/*!
	 * Create a host-visible buffer for an upload, released once the upload commands have been executed
	 *
	 * \return the mapped memory of the buffer, nullptr on failure
	 */
	void * createStagingBuffer(VkDeviceSize size, VkBuffer & buffer);

	//! Submit the recorded uploads and wait for them to finish
	void flushUploads();

	//! \return a sampler for the texture stage settings, cached
	VkSampler getSampler(TextureStage::WrapMode wrapMode, TextureStage::FilterMode minFilter,
	                     TextureStage::FilterMode magFilter, bool mipmaps, float lodBias);

	/*!
	 * Release a resource once the GPU no longer uses it
	 *
	 * Resources used by the frame being recorded are released after it finished on the GPU, otherwise after the last
	 * submitted frame finished.
	 */
	void deferRelease(std::function<void()> release);

	// VR, see VRGraphicsVulkan.cpp

	VkInstance instance() const { return m_instance; }
	VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
	uint32_t queueFamily() const { return m_queueFamily; }
	VkFormat colorFormat() const { return m_colorFormat; }
	//! The instance and device were created by the VR runtime, see vulkan::setExternalDevice()
	bool hasExternalDevice() const { return m_hasExternalDevice; }

	/*!
	 * Replicate every draw call of the 3D scene into the views of a stereo target, like the Direct3D 11 backend.
	 * The scene is rendered with the eyes side by side, it stays bound after \ref endScene() for the overlays drawn
	 * in the world (pointer, hand menus) until \ref finishStereoScene().
	 *
	 * \param views The views to draw into, or an empty list to disable
	 * \param size  The size of the stereo output with both eyes side by side
	 */
	void setStereoViews(std::vector<StereoView> views, const Vec2i & size);

	//! Apply the effects to the stereo scene and draw it into the stereo output, false if no scene was rendered
	bool finishStereoScene();
	//! Copy one eye of the stereo output into an image of the VR runtime (in the color attachment layout)
	void copyStereoEyeToImage(size_t eye, VkImage image);

	/*!
	 * The interface layer: everything except the 3D scene is rendered into this target of the window size instead
	 * of the window while it is active
	 */
	bool createOutputTarget(const Vec2i & size);
	void destroyOutputTarget();
	bool hasOutputTarget() const { return m_outputColor.image != VK_NULL_HANDLE; }
	void setOutputTargetActive(bool active);
	void copyOutputToImage(VkImage image);
	bool copyOutputToTexture(Texture * texture, const Rect & area);
	//! Copy the window into an image of the VR runtime, for frames drawn directly to the window
	void copyWindowToImage(VkImage image);

	//! Write premultiplied alpha coverage to the alpha channel, like the Direct3D 11 backend
	void setPremultipliedAlphaOutput(bool enable);

	struct OverlayVertex {
		float position[4]; //!< Clip space
		float uv[2];
	};

	enum OverlaySource {
		StereoOutput, //!< Both eyes side by side, see finishStereoScene()
		OutputTarget  //!< The interface layer, see createOutputTarget()
	};

	/*!
	 * Draw a textured triangle strip into the window, for the VR desktop view
	 *
	 * \param blend Blend the premultiplied alpha texture over the window instead of replacing it
	 */
	void drawOverlay(OverlaySource source, const OverlayVertex * vertices, size_t count, bool blend);

	//! Clear the window, also while the interface layer is active
	void clearWindow();

	//! Submit the commands recorded so far, images of the VR runtime must be written before they are released
	void submitCommands();

private:

	struct ImageTarget {
		VkImage image = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	};

	//! Host-visible memory for data used by one frame, grows by adding chunks
	struct RingBuffer {
		struct Chunk {
			VkBuffer buffer = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			u8 * data = nullptr;
			VkDeviceSize size = 0;
		};
		VkBufferUsageFlags usage = 0;
		VkDeviceSize chunkSize = 0;
		std::vector<Chunk> chunks;
		size_t current = 0;
		VkDeviceSize pos = 0;
	};

	struct RingAllocation {
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceSize offset = 0;
		void * data = nullptr;
	};

	struct Frame {
		VkCommandPool commandPool = VK_NULL_HANDLE;
		VkCommandBuffer commands = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkSemaphore imageAcquired = VK_NULL_HANDLE;
		std::vector<std::function<void()>> releases;
		RingBuffer vertices;
		RingBuffer indices;
		RingBuffer constants;
		std::vector<VkDescriptorPool> descriptorPools;
		size_t descriptorPool = 0;
	};

	//! The resources bound to the descriptor set of a draw
	struct DescriptorKey {
		VkBuffer vsConstants = VK_NULL_HANDLE;
		VkBuffer psConstants = VK_NULL_HANDLE;
		VkBuffer lightConstants = VK_NULL_HANDLE;
		VkImageView views[4] = {};
		VkSampler samplers[3] = {};
		bool operator==(const DescriptorKey & o) const;
	};

	struct UploadContext {
		VkCommandPool commandPool = VK_NULL_HANDLE;
		VkCommandBuffer commands = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		bool recording = false;
		std::vector<std::pair<VkBuffer, VmaAllocation>> staging;
		VkDeviceSize stagingSize = 0;
		VkDescriptorPool descriptorPool = VK_NULL_HANDLE; //!< Reset after the uploads were executed
		std::vector<std::function<void()>> releases; //!< Run after the uploads were executed
	};

	static constexpr size_t FramesInFlight = 2;
	//! Staging memory after which the uploads are submitted early (while loading levels)
	static constexpr VkDeviceSize MaxPendingUploadSize = VkDeviceSize(64) * 1024 * 1024;

	void clearSamplers();

	// Ring buffers, see VulkanRenderer.cpp
	bool allocate(RingBuffer & ring, VkDeviceSize size, VkDeviceSize alignment, RingAllocation & allocation);
	void releaseRing(RingBuffer & ring);
	void flushRing(RingBuffer & ring);
	VkDescriptorSet allocateDescriptorSet(VkDescriptorSetLayout layout);

	// Shaders and pipelines, see VulkanPipelines.cpp
	bool createPipelineResources();
	void releasePipelineResources();
	VkPipeline getPipeline(uint64_t key);

	/*!
	 * Prepare the state of a draw: pipeline, dynamic state, constants and descriptors
	 *
	 * \return false if the draw must be skipped
	 */
	bool prepareDraw(VertexType type, Primitive primitive);
	glm::mat4x4 screenTransform() const;
	//! Copy vertices into the ring buffer of the frame and bind them
	bool bindVertices(VertexType type, const void * vertices, size_t count);
	//! Bind the copy of a vertex buffer in the current frame, copying it first if needed
	bool bindFrameCopy(VertexType type, const void * vertices, size_t count, FrameCopy & copy);
	void bindVertexBuffer(VkBuffer buffer, VkDeviceSize offset);
	bool bindIndices(const unsigned short * indices, size_t count);
	//! Forget the state bound in the command buffer of the frame
	void invalidateBoundState();

	bool createInstance(SDL_Window * window);
	bool selectPhysicalDevice();
	bool createDevice();
	bool createFrames();
	bool createSwapchain();
	void releaseSwapchain();
	bool createSizeDependentResources();
	void releaseSizeDependentResources();
	bool createImageTarget(ImageTarget & target, VkFormat format, VkImageUsageFlags usage,
	                       VkImageAspectFlags aspect, const Vec2i & size, VkSampleCountFlagBits samples);
	void releaseImageTarget(ImageTarget & target);
	bool createSceneTargets(const Vec2i & size);
	void releaseSceneTargets();
	//! Size of the current target relative to the window, the game uses window pixels
	Vec2f targetScale() const;
	Rect toTargetRect(const Rect & rect) const;
	//! Draw the scene into the window target
	// Post-processing of the scene, see VulkanPostProcess.cpp

	//! Descriptor set layout of a full-screen pass: vsBlit with a pixel shader reading one source texture
	struct PostLayout {
		VkDescriptorSetLayout set = VK_NULL_HANDLE;
		VkPipelineLayout pipeline = VK_NULL_HANDLE;
		uint32_t constantsBinding = 0; //!< The constant buffer of the pixel shader, 0 if none
		bool sceneMask = false; //!< The pixel shader reads the scene stencil (t4)
	};
	//! Pixel shader, target format, additive blending, overlay (vsOverlay triangle strip instead of vsBlit)
	typedef std::tuple<VkShaderModule, VkFormat, bool, bool> PostPipelineKey;

	//! A part of a full-screen pass: one eye of the stereo scene, or everything
	struct PostRegion {
		Vec4f rect; //!< In the source and target textures: offset and size in [0, 1]
		const void * constants;
	};

	bool createPostProcessResources();
	void releasePostProcessResources();
	bool createPostLayout(PostLayout & layout, uint32_t constantsBinding, bool sceneMask);
	void releasePostLayout(PostLayout & layout);
	//! \param format VK_FORMAT_UNDEFINED for the window target
	VkPipeline getPostPipeline(VkShaderModule pixelShader, const char * entry, const PostLayout & layout,
	                           VkFormat format, bool additive, bool overlay = false);
	/*!
	 * Bind the pipeline and descriptors of a full-screen pass in the current rendering and draw it into a part of
	 * the target
	 */
	bool drawPostPass(VkPipeline pipeline, const PostLayout & layout, VkImageView source, const Vec4f & sourceRect,
	                  const void * constants, size_t constantsSize, const Vec2i & targetSize,
	                  const Vec4f & targetRect);
	//! A full-screen pass from one image into another, for each region
	void postPass(ImageTarget & target, VkFormat format, const Vec2i & targetSize, ImageTarget & source,
	              VkShaderModule shader, const char * entry, const PostLayout & layout,
	              const std::vector<PostRegion> & regions, size_t constantsSize, bool additive);
	//! The views of the scene target: both eyes of the stereo scene, or the whole target
	std::vector<Vec4f> sceneRegions() const;
	void renderBloom(ImageTarget & scene);
	//! Draw the scene into a target with the color grading, the window target for VK_FORMAT_UNDEFINED
	void composeScene(ImageTarget & scene, ImageTarget * target, VkFormat format, const Vec2i & size,
	                  bool vignette);

	//! \return true while the 3D scene is drawn into the stereo target
	bool stereoActive() const { return m_inScene && !m_stereoViews.empty(); }
	//! The size of the target that is rendered into
	Vec2i targetSize() const { return m_inScene ? m_sceneSize : m_outputActive ? m_outputSize : m_size; }
	//! Record a draw once, or once for each eye of the stereo scene with its constants and viewport
	template <class DrawFunction>
	void drawViews(const DrawFunction & draw);
	//! Copy a part of an image into an image of the VR runtime (in the color attachment layout)
	void copyToRuntimeImage(ImageTarget & source, const Rect & area, VkImage image);
	VkPipeline getReliefPipeline();
	VkDescriptorSet allocateUploadDescriptorSet(VkDescriptorSetLayout layout);
	//! Generate or release the relief maps of all textures after the setting changed
	void updateReliefMaps(bool wasEnabled);

	void reinit();
	void shutdown();

	//! Start recording the commands of the current frame
	void beginFrame();
	//! Submit the commands of the current frame
	void endFrame(VkSemaphore wait, VkPipelineStageFlags waitStage, VkSemaphore signal);
	void beginRendering();
	void endRendering();
	void transition(ImageTarget & target, VkImageLayout layout, VkImageAspectFlags aspect);

	SDL_Window * m_window;

	std::string m_snapshotPath; //!< ARX_VULKAN_SNAPSHOT
	std::chrono::steady_clock::time_point m_snapshotTime;

	VkInstance m_instance;
	vulkan::ExternalDevice * m_externalDevice; //!< Only while creating the device
	bool m_hasExternalDevice;
	VkDebugUtilsMessengerEXT m_debugMessenger;
	VkSurfaceKHR m_surface;
	VkPhysicalDevice m_physicalDevice;
	VkPhysicalDeviceProperties m_properties;
	uint32_t m_apiVersion; //!< The Vulkan version used by both the instance and the device
	bool m_dynamicRenderingExtension; //!< VK_KHR_dynamic_rendering instead of Vulkan 1.3
	VkPhysicalDeviceFeatures m_features; //!< Features enabled on the device
	uint32_t m_queueFamily;
	VkDevice m_device;
	VkQueue m_queue;
	VmaAllocator m_allocator;
	PFN_vkCmdBeginRenderingKHR m_cmdBeginRendering;
	PFN_vkCmdEndRenderingKHR m_cmdEndRendering;

	VkSwapchainKHR m_swapchain;
	VkExtent2D m_swapchainExtent;
	std::vector<VkImage> m_swapchainImages;
	std::vector<VkSemaphore> m_renderFinished; //!< One for each swapchain image
	VkPresentModeKHR m_presentMode;
	bool m_swapchainDirty;

	VkFormat m_colorFormat; //!< Same as the swapchain so that the target can be copied or resolved directly
	VkFormat m_depthFormat;
	VkSampleCountFlagBits m_msaaSamples;
	ImageTarget m_colorTarget;
	ImageTarget m_depthTarget;
	Vec2i m_size;

	// The 3D scene, rendered at a lower resolution, see beginScene()
	float m_sceneScale;
	bool m_inScene;
	Vec2i m_sceneSize;
	ImageTarget m_sceneColor;
	ImageTarget m_sceneDepth;
	ImageTarget m_sceneResolved; //!< Only with MSAA, resolved at the end of rendering

	Frame m_frames[FramesInFlight];
	uint64_t m_frameSerial; //!< Unique number of the frame being recorded, see FrameCopy
	UploadContext m_upload;
	std::unordered_map<uint64_t, VkSampler> m_samplers;
	size_t m_frameIndex;
	bool m_frameActive;
	bool m_renderingActive;
	bool m_deviceLost;

	int m_vsync;
	bool m_hasMSAA;
	bool m_wireframe;
	float m_maximumAnisotropy;
	float m_maximumSupportedAnisotropy;

	Rect m_viewport;
	Rect m_scissor;
	glm::mat4x4 m_view;
	glm::mat4x4 m_projection;
	Color4f m_fogColor;
	float m_fogStart;
	float m_fogEnd;

	typedef boost::intrusive::list<VulkanTexture, boost::intrusive::constant_time_size<false>> TextureList;
	TextureList m_textures;

	// Shaders and pipelines
	VkShaderModule m_vertexShaders[VertexTypeCount];
	VkShaderModule m_pixelShader;
	VkShaderModule m_pixelShaderSampleShading;
	VkDescriptorSetLayout m_descriptorSetLayout;
	VkPipelineLayout m_pipelineLayout;
	VkPipelineCache m_pipelineCache;
	std::unordered_map<uint64_t, VkPipeline> m_pipelines;
	VkShaderModule m_blitVertexShader;
	VkShaderModule m_blitPixelShader;
	PostLayout m_blitLayout;
	PostLayout m_bloomPrefilterLayout;
	PostLayout m_bloomLayout;
	PostLayout m_gradingLayout;
	std::map<PostPipelineKey, VkPipeline> m_postPipelines;
	VkShaderModule m_bloomPrefilterShader;
	VkShaderModule m_bloomDownsampleShader;
	VkShaderModule m_bloomUpsampleShader;
	VkShaderModule m_bloomCompositeShader;
	VkShaderModule m_gradingShader;
	ImageTarget m_noSceneMask; //!< Bound as the scene stencil: no pixel is excluded from the effects
	VkShaderModule m_overlayVertexShader;

	// VR
	std::vector<StereoView> m_stereoViews;
	Vec2i m_stereoSize; //!< Both eyes side by side
	Vec2i m_stereoEyeSize; //!< One eye in the scene target, with the render scale
	ImageTarget m_stereoOutput;
	uint32_t m_stereoVSOffsets[2]; //!< The VS constants of each eye, see drawViews()
	Vec2i m_outputSize;
	ImageTarget m_outputColor;
	ImageTarget m_outputDepth;
	ImageTarget m_outputResolved; //!< Only with MSAA, resolved at the end of rendering
	bool m_outputActive;
	bool m_premultipliedAlpha;

	// Bloom and color grading of the 3D scene
	static constexpr size_t BloomLevels = 6;
	float m_bloomStrength;
	bool m_bloom;
	VkFormat m_bloomFormat;
	ImageTarget m_bloomTargets[BloomLevels]; //!< From half the scene size down
	Vec2i m_bloomSizes[BloomLevels];
	size_t m_colorGrading; //!< Preset index, 0 = off
	VkShaderModule m_reliefPixelShader;
	VkPipeline m_reliefPipeline;
	VkBuffer m_fullRectConstants; //!< BlitConstants for vsBlit covering the whole source
	VmaAllocation m_fullRectAllocation;

	// Effects of the 3D scene, like the Direct3D 11 backend
	bool m_pixelLighting;
	float m_normalMapStrength;
	float m_parallaxStrength;
	float m_specularStrength;
	bool m_waterSurface;
	bool m_drawingScene; //!< Between beginScene() and endScene(), also without the scene targets
	float m_effectTime; //!< Seconds, repeating, for the lava
	std::vector<u8> m_lightData; //!< LightConstants, see setPixelLights()
	size_t m_lightDataSize; //!< The part of m_lightData used by the lights
	FrameCopy m_lights; //!< m_lightData in the memory of the current frame
	std::unique_ptr<VulkanTexture> m_whiteTexture; //!< Bound to the texture slots without a texture

	// State bound in the command buffer of the current frame
	VkPipeline m_boundPipeline;
	VkBuffer m_boundVertexBuffer;
	VkDeviceSize m_boundVertexOffset;
	VkBuffer m_boundIndexBuffer;
	VkDeviceSize m_boundIndexOffset;
	bool m_dynamicStateValid;
	float m_boundDepthBias;
	DescriptorKey m_boundDescriptors;
	VkDescriptorSet m_boundDescriptorSet;
	uint32_t m_boundConstantOffsets[3]; //!< VS, PS and light constants
	RingAllocation m_vsConstants;
	RingAllocation m_psConstants;
	std::vector<u8> m_vsConstantsData;
	std::vector<u8> m_psConstantsData;

};

#endif // ARX_GRAPHICS_VULKAN_VULKANRENDERER_H
