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

#include <functional>
#include <memory>
#include <vector>

#include <volk.h>
#include <vk_mem_alloc.h>

#include <boost/intrusive/list.hpp>

#include "graphics/Color.h"
#include "graphics/Renderer.h"
#include "graphics/vulkan/VulkanTexture.h"
#include "math/Rectangle.h"
#include "platform/Platform.h"

struct SDL_Window;
class VulkanTextureStage;

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

	VulkanTextureStage * GetTextureStage(size_t textureStage) {
		return reinterpret_cast<VulkanTextureStage *>(Renderer::GetTextureStage(textureStage));
	}

	void setVSync(int vsync);
	void present();

	int msaaSamples() const { return int(m_msaaSamples); }

	//! Draw vertices of a vertex buffer
	void drawBuffer(VertexType type, Primitive primitive, const void * vertices, size_t count);
	void drawBufferIndexed(VertexType type, Primitive primitive, const void * vertices,
	                       const unsigned short * indices, size_t nindices);

	void onTextureDestroyed(VulkanTexture * texture);

	/*!
	 * Release a resource once the GPU no longer uses it
	 *
	 * Resources used by the frame being recorded are released after it finished on the GPU, otherwise after the last
	 * submitted frame finished.
	 */
	void deferRelease(std::function<void()> release);

private:

	struct ImageTarget {
		VkImage image = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	};

	struct Frame {
		VkCommandPool commandPool = VK_NULL_HANDLE;
		VkCommandBuffer commands = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkSemaphore imageAcquired = VK_NULL_HANDLE;
		std::vector<std::function<void()>> releases;
	};

	static constexpr size_t FramesInFlight = 2;

	bool createInstance(SDL_Window * window);
	bool selectPhysicalDevice();
	bool createDevice();
	bool createFrames();
	bool createSwapchain();
	void releaseSwapchain();
	bool createSizeDependentResources();
	void releaseSizeDependentResources();
	bool createImageTarget(ImageTarget & target, VkFormat format, VkImageUsageFlags usage,
	                       VkImageAspectFlags aspect);
	void releaseImageTarget(ImageTarget & target);

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

	VkInstance m_instance;
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

	Frame m_frames[FramesInFlight];
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

};

#endif // ARX_GRAPHICS_VULKAN_VULKANRENDERER_H
