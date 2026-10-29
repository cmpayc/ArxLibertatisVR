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

// Effects of the 3D scene after it was rendered (bloom, color grading), the copy into the window or the VR stereo
// output, and the overlays of the VR desktop view

#include "graphics/vulkan/VulkanRenderer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <vector>

#include "io/log/Logger.h"

// SPIR-V compiled from D3D11Shaders.h by cmake/SPIRVShaders.cmake
#include "psBloomPrefilter.h"
#include "psBloomPrefilterMS.h"
#include "psBloomDownsample.h"
#include "psBloomUpsample.h"
#include "psBloomComposite.h"
#include "psColorGrading.h"
#include "vsOverlay.h"

namespace {

// Bindings of the full-screen passes (vsBlit and the pixel shaders), see VulkanPipelines.cpp
const uint32_t BlitConstantsBinding = 8;  // VS b8
const uint32_t BloomConstantsBinding = 64 + 4; // PS b4
const uint32_t GradingConstantsBinding = 64 + 5; // PS b5
const uint32_t SourceBinding = 64 + 16; // PS t0
const uint32_t SceneMaskBinding = 64 + 16 + 4; // PS t4
const uint32_t SamplerBinding = 64 + 48; // PS s0

struct BlitConstants {
	float rect[4]; // The part of the source textures covered by the viewport: offset and size
};

// Same as the Direct3D 11 backend

struct BloomConstants {
	float params[4];
	float source[4];
	float clamp[4]; // The source texture coordinates of the view: xy = minimum, zw = maximum
	float fade[4];  // The distance fade of Direct3D 11, unused (no scene depth bound for the prefilter)
};

struct GradingConstants {
	float params[4];
	float tint[4];
	float shadows[4];
	float highlights[4];
	float tone[4];
};

struct ColorGradingPreset {
	const char * name;
	float exposure;
	float gamma; //!< Below 1 brightens dark areas
	float contrast;
	float saturation;
	float vignette;
	float tint[3];
	float shadows[3];
	float highlights[3];
};

// Contrast pivot, low because Arx scenes are dark
const float ColorGradingPivot = 0.2f;

// The first preset leaves the colors unchanged, the others are slightly brighter than the original
const ColorGradingPreset ColorGradingPresets[] = {
	{ "off", 1.f, 1.f, 1.f, 1.f, 0.f, { 1.f, 1.f, 1.f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f } },
	{ "natural", 1.f, 0.95f, 1.06f, 1.15f, 0.f, { 1.f, 1.f, 1.f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f } },
	{ "warm", 1.f, 0.95f, 1.05f, 1.1f, 0.1f, { 1.06f, 1.f, 0.9f }, { 0.01f, 0.f, -0.01f },
	  { 0.02f, 0.01f, -0.01f } },
	{ "cinematic", 1.f, 0.95f, 1.1f, 0.95f, 0.2f, { 1.f, 1.f, 1.f }, { -0.012f, 0.004f, 0.02f },
	  { 0.03f, 0.01f, -0.02f } },
};

// Bloom settings: brightness where the bloom starts, width of the soft transition
const float BloomThreshold = 0.45f;
const float BloomKnee = 0.2f;

const Vec4f FullRect(0.f, 0.f, 1.f, 1.f);

VkShaderModule createModule(VkDevice device, const unsigned char * code, size_t size, const char * name) {
	std::vector<uint32_t> words((size + 3) / 4);
	std::memcpy(words.data(), code, size);
	VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	info.codeSize = size;
	info.pCode = words.data();
	VkShaderModule module = VK_NULL_HANDLE;
	if(vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
		LogError << "Failed to create Vulkan shader module " << name;
		return VK_NULL_HANDLE;
	}
	return module;
}

} // anonymous namespace

bool VulkanRenderer::createPostLayout(PostLayout & layout, uint32_t constantsBinding, bool sceneMask) {

	std::vector<VkDescriptorSetLayoutBinding> bindings;
	auto addBinding = [&bindings](uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages) {
		VkDescriptorSetLayoutBinding entry = {};
		entry.binding = binding;
		entry.descriptorType = type;
		entry.descriptorCount = 1;
		entry.stageFlags = stages;
		bindings.push_back(entry);
	};
	addBinding(BlitConstantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_VERTEX_BIT);
	if(constantsBinding) {
		addBinding(constantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_FRAGMENT_BIT);
	}
	addBinding(SourceBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT);
	if(sceneMask) {
		addBinding(SceneMaskBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT);
	}
	addBinding(SamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);

	VkDescriptorSetLayoutCreateInfo setLayout = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	setLayout.bindingCount = uint32_t(bindings.size());
	setLayout.pBindings = bindings.data();
	if(vkCreateDescriptorSetLayout(m_device, &setLayout, nullptr, &layout.set) != VK_SUCCESS) {
		return false;
	}

	VkPipelineLayoutCreateInfo pipelineLayout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipelineLayout.setLayoutCount = 1;
	pipelineLayout.pSetLayouts = &layout.set;
	if(vkCreatePipelineLayout(m_device, &pipelineLayout, nullptr, &layout.pipeline) != VK_SUCCESS) {
		return false;
	}

	layout.constantsBinding = constantsBinding;
	layout.sceneMask = sceneMask;

	return true;
}

void VulkanRenderer::releasePostLayout(PostLayout & layout) {
	vkDestroyPipelineLayout(m_device, layout.pipeline, nullptr);
	vkDestroyDescriptorSetLayout(m_device, layout.set, nullptr);
	layout = PostLayout();
}

bool VulkanRenderer::createPostProcessResources() {

	m_bloomPrefilterShader = createModule(m_device, spirv_psBloomPrefilter, sizeof(spirv_psBloomPrefilter),
	                                      "psBloomPrefilter");
	m_bloomPrefilterShaderMS = createModule(m_device, spirv_psBloomPrefilterMS, sizeof(spirv_psBloomPrefilterMS),
	                                        "psBloomPrefilterMS");
	m_bloomDownsampleShader = createModule(m_device, spirv_psBloomDownsample, sizeof(spirv_psBloomDownsample),
	                                       "psBloomDownsample");
	m_bloomUpsampleShader = createModule(m_device, spirv_psBloomUpsample, sizeof(spirv_psBloomUpsample),
	                                     "psBloomUpsample");
	m_bloomCompositeShader = createModule(m_device, spirv_psBloomComposite, sizeof(spirv_psBloomComposite),
	                                      "psBloomComposite");
	m_gradingShader = createModule(m_device, spirv_psColorGrading, sizeof(spirv_psColorGrading), "psColorGrading");
	m_overlayVertexShader = createModule(m_device, spirv_vsOverlay, sizeof(spirv_vsOverlay), "vsOverlay");

	if(!createPostLayout(m_blitLayout, 0, false)
	   || !createPostLayout(m_bloomPrefilterLayout, BloomConstantsBinding, true)
	   || !createPostLayout(m_bloomLayout, BloomConstantsBinding, false)
	   || !createPostLayout(m_gradingLayout, GradingConstantsBinding, false)) {
		LogError << "Failed to create the Vulkan post-processing layouts";
		return false;
	}

	// The bloom is added up in a format with more range than the scene, like R11G11B10 in Direct3D 11
	m_bloomFormat = VK_FORMAT_UNDEFINED;
	for(VkFormat format : { VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT, m_colorFormat }) {
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &properties);
		VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
		                              | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
		if((properties.optimalTilingFeatures & needed) == needed) {
			m_bloomFormat = format;
			break;
		}
	}

	// The stencil of the scene marks the pixels without effects (VR only for now): none is marked
	{
		Vec2i size(1, 1);
		VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
		if(!createImageTarget(m_noSceneMask, VK_FORMAT_R8G8_UINT,
		                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		                      VK_IMAGE_ASPECT_COLOR_BIT, size, samples)) {
			return false;
		}
		VkCommandBuffer commands = beginUpload();
		VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = m_noSceneMask.image;
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
		                     nullptr, 0, nullptr, 1, &barrier);
		VkClearColorValue zero = {};
		vkCmdClearColorImage(commands, m_noSceneMask.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
		                     &barrier.subresourceRange);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
		                     nullptr, 0, nullptr, 1, &barrier);
		m_noSceneMask.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	return true;
}

void VulkanRenderer::releasePostProcessResources() {

	for(const auto & entry : m_postPipelines) {
		vkDestroyPipeline(m_device, entry.second, nullptr);
	}
	m_postPipelines.clear();

	releasePostLayout(m_blitLayout);
	releasePostLayout(m_bloomPrefilterLayout);
	releasePostLayout(m_bloomLayout);
	releasePostLayout(m_gradingLayout);

	for(VkShaderModule * module : { &m_bloomPrefilterShader, &m_bloomPrefilterShaderMS, &m_bloomDownsampleShader,
	                                &m_bloomUpsampleShader,
	                                &m_bloomCompositeShader, &m_gradingShader, &m_overlayVertexShader }) {
		vkDestroyShaderModule(m_device, *module, nullptr);
		*module = VK_NULL_HANDLE;
	}

	releaseImageTarget(m_noSceneMask);
	releaseImageTarget(m_stereoOutput);
	destroyOutputTarget();

}

VkPipeline VulkanRenderer::getPostPipeline(VkShaderModule pixelShader, const char * entry, const PostLayout & layout,
                                           VkFormat format, bool additive, bool overlay) {

	PostPipelineKey key(pixelShader, format, additive, overlay);
	auto it = m_postPipelines.find(key);
	if(it != m_postPipelines.end()) {
		return it->second;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }
	};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = overlay ? m_overlayVertexShader : m_blitVertexShader;
	stages[0].pName = overlay ? "vsOverlay" : "vsBlit";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = pixelShader;
	stages[1].pName = entry;

	// A full-screen triangle from SV_VertexID, or a triangle strip with clip space positions (OverlayVertex)
	VkVertexInputBindingDescription binding = { 0, uint32_t(sizeof(OverlayVertex)), VK_VERTEX_INPUT_RATE_VERTEX };
	const VkVertexInputAttributeDescription attributes[] = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, uint32_t(offsetof(OverlayVertex, position)) },
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(OverlayVertex, uv)) },
	};
	VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	if(overlay) {
		vertexInput.vertexBindingDescriptionCount = 1;
		vertexInput.pVertexBindingDescriptions = &binding;
		vertexInput.vertexAttributeDescriptionCount = uint32_t(std::size(attributes));
		vertexInput.pVertexAttributeDescriptions = attributes;
	}
	VkPipelineInputAssemblyStateCreateInfo inputAssembly
		= { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	inputAssembly.topology = overlay ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rasterization
		= { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.lineWidth = 1.f;

	// VK_FORMAT_UNDEFINED: the window target, with the same attachments as the scene pipelines (see beginRendering)
	bool window = (format == VK_FORMAT_UNDEFINED);
	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = window ? m_msaaSamples : VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

	VkPipelineColorBlendAttachmentState blend = {};
	blend.blendEnable = additive ? VK_TRUE : VK_FALSE;
	blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	// Overlays blend premultiplied alpha images over the window, the effects add up
	VkBlendFactor destination = overlay ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
	blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = destination;
	blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
	blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
	                       | VK_COLOR_COMPONENT_A_BIT;
	const VkFormat windowFormats[] = { m_colorFormat, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
	const VkPipelineColorBlendAttachmentState blends[] = { blend, blend, blend, blend };
	VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	colorBlend.attachmentCount = window ? uint32_t(std::size(blends)) : 1;
	colorBlend.pAttachments = blends;

	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.dynamicStateCount = uint32_t(std::size(dynamicStates));
	dynamic.pDynamicStates = dynamicStates;

	VkPipelineRenderingCreateInfoKHR rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	if(window) {
		rendering.colorAttachmentCount = uint32_t(std::size(windowFormats));
		rendering.pColorAttachmentFormats = windowFormats;
		rendering.depthAttachmentFormat = m_depthFormat;
		rendering.stencilAttachmentFormat = m_depthFormat;
	} else {
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachmentFormats = &format;
	}

	VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	info.pNext = &rendering;
	info.stageCount = uint32_t(std::size(stages));
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &inputAssembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &rasterization;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depthStencil;
	info.pColorBlendState = &colorBlend;
	info.pDynamicState = &dynamic;
	info.layout = layout.pipeline;

	VkPipeline pipeline = VK_NULL_HANDLE;
	if(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
		LogError << "Failed to create the Vulkan pipeline for " << entry;
		pipeline = VK_NULL_HANDLE;
	}

	// Also remember failures so that they are not retried for every frame
	m_postPipelines.emplace(key, pipeline);
	return pipeline;
}

bool VulkanRenderer::drawPostPass(VkPipeline pipeline, const PostLayout & layout, VkImageView source,
                                  const Vec4f & sourceRect, const void * constants, size_t constantsSize,
                                  const Vec2i & targetSize, const Vec4f & targetRect) {

	VkDeviceSize alignment = std::max<VkDeviceSize>(m_properties.limits.minUniformBufferOffsetAlignment, 16);

	VkDescriptorSet set = pipeline ? allocateDescriptorSet(layout.set) : VK_NULL_HANDLE;
	RingAllocation blitConstants;
	RingAllocation passConstants;
	if(!set || !allocate(m_frames[m_frameIndex].constants, sizeof(BlitConstants), alignment, blitConstants)
	   || (layout.constantsBinding
	       && !allocate(m_frames[m_frameIndex].constants, constantsSize, alignment, passConstants))) {
		return false;
	}
	BlitConstants blit = { { sourceRect.x, sourceRect.y, sourceRect.z, sourceRect.w } };
	std::memcpy(blitConstants.data, &blit, sizeof(blit));
	if(layout.constantsBinding) {
		std::memcpy(passConstants.data, constants, constantsSize);
	}

	VkDescriptorBufferInfo blitBuffer = { blitConstants.buffer, 0, sizeof(BlitConstants) };
	VkDescriptorBufferInfo passBuffer = { passConstants.buffer, 0, VkDeviceSize(constantsSize) };
	VkDescriptorImageInfo image = { VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo mask = { VK_NULL_HANDLE, m_noSceneMask.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	if(m_postSceneMask) {
		mask = { VK_NULL_HANDLE, m_postSceneMask, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
	}
	VkDescriptorImageInfo sampler = {
		getSampler(TextureStage::WrapClamp, TextureStage::FilterLinear, TextureStage::FilterLinear, false, 0.f),
		VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED
	};

	std::vector<VkWriteDescriptorSet> writes;
	auto addWrite = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo * buffer,
	                    const VkDescriptorImageInfo * imageInfo) {
		VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet = set;
		write.dstBinding = binding;
		write.descriptorCount = 1;
		write.descriptorType = type;
		write.pBufferInfo = buffer;
		write.pImageInfo = imageInfo;
		writes.push_back(write);
	};
	addWrite(BlitConstantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, &blitBuffer, nullptr);
	if(layout.constantsBinding) {
		addWrite(layout.constantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, &passBuffer, nullptr);
	}
	addWrite(SourceBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, nullptr, &image);
	if(layout.sceneMask) {
		addWrite(SceneMaskBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, nullptr, &mask);
	}
	addWrite(SamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER, nullptr, &sampler);
	vkUpdateDescriptorSets(m_device, uint32_t(writes.size()), writes.data(), 0, nullptr);

	VkCommandBuffer commands = m_frames[m_frameIndex].commands;

	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	// Dynamic offsets in the order of the bindings: VS b8, then the PS constants
	uint32_t offsets[2] = { uint32_t(blitConstants.offset), uint32_t(passConstants.offset) };
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.pipeline, 0, 1, &set,
	                        layout.constantsBinding ? 2 : 1, offsets);

	// Negative height: the y axis points up in clip space like in Direct3D
	float x = targetRect.x * float(targetSize.x);
	float y = targetRect.y * float(targetSize.y);
	float width = targetRect.z * float(targetSize.x);
	float height = targetRect.w * float(targetSize.y);
	VkViewport viewport = { x, y + height, width, -height, 0.f, 1.f };
	vkCmdSetViewport(commands, 0, 1, &viewport);
	VkRect2D scissor = { { s32(x + 0.5f), s32(y + 0.5f) }, { uint32_t(width + 0.5f), uint32_t(height + 0.5f) } };
	vkCmdSetScissor(commands, 0, 1, &scissor);

	return true;
}

void VulkanRenderer::postPass(ImageTarget & target, VkFormat format, const Vec2i & targetSize, ImageTarget & source,
                              VkShaderModule shader, const char * entry, const PostLayout & layout,
                              const std::vector<PostRegion> & regions, size_t constantsSize, bool additive) {

	endRendering();

	transition(source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
	transition(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

	VkRenderingAttachmentInfoKHR attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
	attachment.imageView = target.view;
	attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachment.loadOp = additive ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfoKHR rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
	rendering.renderArea = { { 0, 0 }, { uint32_t(targetSize.x), uint32_t(targetSize.y) } };
	rendering.layerCount = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments = &attachment;

	VkCommandBuffer commands = m_frames[m_frameIndex].commands;
	m_cmdBeginRendering(commands, &rendering);

	VkPipeline pipeline = getPostPipeline(shader, entry, layout, format, additive);
	for(const PostRegion & region : regions) {
		if(drawPostPass(pipeline, layout, source.view, region.rect, region.constants, constantsSize, targetSize,
		                region.rect)) {
			vkCmdDraw(commands, 3, 1, 0, 0);
		}
	}

	m_cmdEndRendering(commands);

}

void VulkanRenderer::drawToRuntimeImage(ImageTarget & source, const Vec2i & sourceSize, const Rect & area,
                                        VkImage image) {

	if(!source.image || !image || area.width() <= 0 || area.height() <= 0 || sourceSize.x <= 0 || sourceSize.y <= 0) {
		return;
	}

	beginFrame();

	/*
	 * The runtime image is sRGB (see VRGraphicsVulkan::selectColorFormat) and created with a mutable format: drawn
	 * through a view with the renderer's format, the gamma-encoded bytes are stored unchanged like with a copy
	 */
	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = m_colorFormat;
	viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	ImageTarget target;
	target.image = image;
	target.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	if(vkCreateImageView(m_device, &viewInfo, nullptr, &target.view) != VK_SUCCESS) {
		LogError << "Failed to create a Vulkan view of an OpenXR image";
		copyToRuntimeImage(source, area, image);
		return;
	}
	VkDevice device = m_device;
	VkImageView view = target.view;
	deferRelease([device, view]() {
		vkDestroyImageView(device, view, nullptr);
	});

	Vec2i size(area.width(), area.height());
	Vec4f sourceRect(float(area.left) / float(sourceSize.x), float(area.top) / float(sourceSize.y),
	                 float(size.x) / float(sourceSize.x), float(size.y) / float(sourceSize.y));

	endRendering();
	transition(source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

	VkRenderingAttachmentInfoKHR attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
	attachment.imageView = target.view;
	attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfoKHR rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
	rendering.renderArea = { { 0, 0 }, { uint32_t(size.x), uint32_t(size.y) } };
	rendering.layerCount = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments = &attachment;

	VkCommandBuffer commands = m_frames[m_frameIndex].commands;
	m_cmdBeginRendering(commands, &rendering);
	VkPipeline pipeline = getPostPipeline(m_blitPixelShader, "psBlit", m_blitLayout, m_colorFormat, false);
	if(drawPostPass(pipeline, m_blitLayout, source.view, sourceRect, nullptr, 0, size, FullRect)) {
		vkCmdDraw(commands, 3, 1, 0, 0);
		m_stats.draws++;
	}
	m_cmdEndRendering(commands);

	// The next draw binds its own state
	invalidateBoundState();

}

std::vector<Vec4f> VulkanRenderer::sceneRegions() const {

	if(m_stereoViews.empty()) {
		return { FullRect };
	}

	// Each eye of the stereo scene is filtered on its own
	std::vector<Vec4f> regions;
	float width = 1.f / float(m_stereoViews.size());
	for(size_t eye = 0; eye < m_stereoViews.size(); eye++) {
		regions.emplace_back(float(eye) * width, 0.f, width, 1.f);
	}
	return regions;
}

void VulkanRenderer::renderBloom(ImageTarget & scene) {

	if(!m_bloomTargets[0].image || !m_bloomPrefilterShader || !m_bloomDownsampleShader || !m_bloomUpsampleShader
	   || !m_bloomCompositeShader) {
		return;
	}

	std::vector<Vec4f> rects = sceneRegions();
	std::vector<BloomConstants> constants(rects.size());
	std::vector<PostRegion> regions(rects.size());

	// The same part of every level, the samples are clamped to it
	auto pass = [&](ImageTarget & target, const Vec2i & targetSize, ImageTarget & source, const Vec2i & sourceSize,
	                VkShaderModule shader, const char * entry, const PostLayout & layout, VkFormat format,
	                bool additive) {
		for(size_t i = 0; i < rects.size(); i++) {
			const Vec4f & rect = rects[i];
			BloomConstants & bloom = constants[i];
			bloom = BloomConstants();
			bloom.params[0] = BloomThreshold;
			bloom.params[1] = BloomKnee;
			// The upsampled levels add up, normalize the result
			bloom.params[2] = 6.f * m_bloomStrength / float(BloomLevels);
			bloom.source[0] = 1.f / float(sourceSize.x);
			bloom.source[1] = 1.f / float(sourceSize.y);
			bloom.source[2] = float(sourceSize.x);
			bloom.source[3] = float(sourceSize.y);
			bloom.clamp[0] = rect.x + 0.5f * bloom.source[0];
			bloom.clamp[1] = rect.y + 0.5f * bloom.source[1];
			bloom.clamp[2] = rect.x + rect.z - 0.5f * bloom.source[0];
			bloom.clamp[3] = rect.y + rect.w - 0.5f * bloom.source[1];
			regions[i] = { rect, &bloom };
		}
		postPass(target, format, targetSize, source, shader, entry, layout, regions, sizeof(BloomConstants), additive);
	};

	/*
	 * Without the pixels marked in the stencil (see setSceneEffectsExcluded): the book held in VR is bright and close
	 * to the eyes, with bloom the whole book glowed (user)
	 */
	VkShaderModule prefilter = m_bloomPrefilterShader;
	const char * prefilterEntry = "psBloomPrefilter";
	if(m_sceneStencilView && (m_sceneSamples == VK_SAMPLE_COUNT_1_BIT || m_bloomPrefilterShaderMS)) {
		if(m_sceneSamples != VK_SAMPLE_COUNT_1_BIT) {
			prefilter = m_bloomPrefilterShaderMS;
		}
		endRendering();
		transition(m_sceneDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
		           VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
		m_postSceneMask = m_sceneStencilView;
	}
	pass(m_bloomTargets[0], m_bloomSizes[0], scene, m_sceneSize, prefilter, prefilterEntry,
	     m_bloomPrefilterLayout, m_bloomFormat, false);
	m_postSceneMask = VK_NULL_HANDLE;
	for(size_t i = 1; i < BloomLevels; i++) {
		pass(m_bloomTargets[i], m_bloomSizes[i], m_bloomTargets[i - 1], m_bloomSizes[i - 1], m_bloomDownsampleShader,
		     "psBloomDownsample", m_bloomLayout, m_bloomFormat, false);
	}
	for(size_t i = BloomLevels - 1; i > 0; i--) {
		pass(m_bloomTargets[i - 1], m_bloomSizes[i - 1], m_bloomTargets[i], m_bloomSizes[i], m_bloomUpsampleShader,
		     "psBloomUpsample", m_bloomLayout, m_bloomFormat, true);
	}

	// Added to the scene
	pass(scene, m_sceneSize, m_bloomTargets[0], m_bloomSizes[0], m_bloomCompositeShader, "psBloomComposite",
	     m_bloomLayout, m_colorFormat, true);

}

void VulkanRenderer::composeScene(ImageTarget & scene, ImageTarget * target, VkFormat format, const Vec2i & size,
                                  bool vignette) {

	transition(scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

	VkCommandBuffer commands = m_frames[m_frameIndex].commands;

	if(target) {
		endRendering();
		transition(*target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
		VkRenderingAttachmentInfoKHR attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
		attachment.imageView = target->view;
		attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingInfoKHR rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
		rendering.renderArea = { { 0, 0 }, { uint32_t(size.x), uint32_t(size.y) } };
		rendering.layerCount = 1;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments = &attachment;
		m_cmdBeginRendering(commands, &rendering);
	} else {
		beginRendering();
		if(!m_renderingActive) {
			return;
		}
	}

	VkPipeline pipeline = VK_NULL_HANDLE;
	const PostLayout * layout = &m_blitLayout;
	GradingConstants constants = { };
	if(m_colorGrading != 0 && m_gradingShader) {
		const ColorGradingPreset & preset = ColorGradingPresets[m_colorGrading];
		constants.params[0] = preset.contrast;
		constants.params[1] = preset.saturation;
		// The lenses darken the edges of the VR view already
		constants.params[2] = vignette ? preset.vignette : 0.f;
		constants.params[3] = float(size.x) / float(std::max<s32>(size.y, 1));
		constants.tone[0] = preset.exposure;
		constants.tone[1] = preset.gamma;
		constants.tone[2] = ColorGradingPivot;
		for(size_t i = 0; i < 3; i++) {
			constants.tint[i] = preset.tint[i];
			constants.shadows[i] = preset.shadows[i];
			constants.highlights[i] = preset.highlights[i];
		}
		layout = &m_gradingLayout;
		pipeline = getPostPipeline(m_gradingShader, "psColorGrading", m_gradingLayout, format, false);
	} else {
		pipeline = getPostPipeline(m_blitPixelShader, "psBlit", m_blitLayout, format, false);
	}

	if(drawPostPass(pipeline, *layout, scene.view, FullRect, &constants, sizeof(constants), size, FullRect)) {
		vkCmdDraw(commands, 3, 1, 0, 0);
	}

	if(target) {
		m_cmdEndRendering(commands);
	}

	// The next draw binds its own state
	invalidateBoundState();

}

bool VulkanRenderer::finishStereoScene() {

	m_drawingScene = false;

	if(!m_inScene || m_stereoViews.empty()) {
		return false;
	}

	// Resolves the multisampled scene, see beginRendering()
	endRendering();
	m_inScene = false;
	invalidateBoundState();

	if(!m_stereoOutput.image) {
		if(!createImageTarget(m_stereoOutput, m_colorFormat,
		                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
		                      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		                      VK_IMAGE_ASPECT_COLOR_BIT, m_stereoSize, VK_SAMPLE_COUNT_1_BIT)) {
			LogError << "Failed to create the " << m_stereoSize.x << "x" << m_stereoSize.y << " Vulkan stereo output";
			return false;
		}
	}

	ImageTarget & scene = m_sceneResolved.image ? m_sceneResolved : m_sceneColor;

	if(m_bloom) {
		renderBloom(scene);
	}

	composeScene(scene, &m_stereoOutput, m_colorFormat, m_stereoSize, false);

	return true;
}

void VulkanRenderer::drawOverlay(OverlaySource source, const OverlayVertex * vertices, size_t count, bool blend) {

	ImageTarget & image = (source == StereoOutput) ? m_stereoOutput
	                      : (m_outputResolved.image ? m_outputResolved : m_outputColor);
	if(!image.image || !vertices || count < 3 || !m_overlayVertexShader || m_inScene || m_outputActive) {
		return;
	}

	endRendering();
	transition(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
	VkImageView texture = image.view;

	beginRendering();
	if(!m_renderingActive) {
		return;
	}

	RingAllocation allocation;
	if(!allocate(m_frames[m_frameIndex].vertices, VkDeviceSize(count * sizeof(OverlayVertex)), 16, allocation)) {
		return;
	}
	std::memcpy(allocation.data, vertices, count * sizeof(OverlayVertex));

	VkPipeline pipeline = getPostPipeline(m_blitPixelShader, "psBlit", m_blitLayout, VK_FORMAT_UNDEFINED, blend, true);
	if(drawPostPass(pipeline, m_blitLayout, texture, FullRect, nullptr, 0, m_size, FullRect)) {
		VkCommandBuffer commands = m_frames[m_frameIndex].commands;
		vkCmdBindVertexBuffers(commands, 0, 1, &allocation.buffer, &allocation.offset);
		vkCmdDraw(commands, uint32_t(count), 1, 0, 0);
	}

	invalidateBoundState();

}

void VulkanRenderer::setBloom(float strength) {

	m_bloomStrength = glm::clamp(strength, 0.f, 1.f);

	bool enabled = (m_bloomStrength > 0.f && m_bloomFormat != VK_FORMAT_UNDEFINED);
	if(enabled == m_bloom) {
		return;
	}

	m_bloom = enabled;

	if(!m_inScene) {
		releaseSceneTargets();
	}

}

void VulkanRenderer::setColorGrading(std::string_view preset) {

	size_t index = 0;
	for(size_t i = 0; i < std::size(ColorGradingPresets); i++) {
		if(preset == ColorGradingPresets[i].name) {
			index = i;
		}
	}

	m_colorGrading = index;

}
