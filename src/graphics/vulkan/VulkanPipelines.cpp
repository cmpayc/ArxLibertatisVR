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

#include "graphics/vulkan/VulkanRenderer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "core/Config.h"
#include "graphics/Vertex.h"
#include "graphics/image/Image.h"
#include "graphics/vulkan/VulkanTexture.h"
#include "graphics/vulkan/VulkanTextureStage.h"
#include "io/fs/FilePath.h"
#include "io/fs/Filesystem.h"
#include "io/fs/SystemPaths.h"
#include "io/log/Logger.h"

// SPIR-V compiled from D3D11Shaders.h by cmake/SPIRVShaders.cmake
#include "vsTL.h"
#include "vsWorld.h"
#include "vsWorld3.h"
#include "psMain.h"
#include "psMainSampleShading.h"
#include "vsBlit.h"
#include "psBlit.h"

/*
 * Shader resources: Direct3D 11 has separate slots for each shader stage, they are mapped to the bindings of
 * descriptor set 0 as <stage base> + <register type offset> + <register> (see cmake/SPIRVShaders.cmake)
 */
namespace {

const uint32_t VertexStage = 0;
const uint32_t PixelStage = 64;
const uint32_t ConstantRegister = 0;
const uint32_t TextureRegister = 16;
const uint32_t SamplerRegister = 48;

const uint32_t VSConstantsBinding = VertexStage + ConstantRegister + 0;
const uint32_t PSConstantsBinding = PixelStage + ConstantRegister + 0;
const uint32_t LightConstantsBinding = PixelStage + ConstantRegister + 6;
const uint32_t TextureBinding = PixelStage + TextureRegister; // texture0-2 and the relief map
const uint32_t SamplerBinding = PixelStage + SamplerRegister; // sampler0-2

const uint32_t BlitConstantsBinding = VertexStage + ConstantRegister + 8;

const size_t TextureStageCount = 3;
const size_t TextureSlotCount = 4;

struct BlitConstants {
	float rect[4]; // The part of the source textures covered by the viewport: offset and size
};

// Same layout as the constant buffers in D3D11Shaders.h (-fvk-use-dx-layout keeps the Direct3D packing rules)

struct VSConstants {
	float transform[16];
	float jitter[4];
	float camera[4]; // World position of the camera
	float modelToWorld[16]; // From pre-transformed model vertices to world space, for per-pixel highlights
	float worldToClip[16]; // From world space to clip space in the 3D scene, for tessellated models
	float prevTransform[16]; // Like transform for the positions of the previous frame (differs for VR eyes)
};

struct PSConstants {
	s32 stages[3][4];
	float fogColor[4];
	float fogParams[4];
	float relief[4];
	float specular[4];
	float surface[4];
	float effect[4];
	float material[4];
};

const size_t MaxPixelLights = 128;

struct LightConstants {
	s32 count[4];
	float lights[MaxPixelLights * 2][4];
};

const VkBlendFactor arxToVkBlendFactor[] = {
	VK_BLEND_FACTOR_ZERO,                // BlendZero,
	VK_BLEND_FACTOR_ONE,                 // BlendOne,
	VK_BLEND_FACTOR_SRC_COLOR,           // BlendSrcColor,
	VK_BLEND_FACTOR_SRC_ALPHA,           // BlendSrcAlpha,
	VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, // BlendInvSrcColor,
	VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, // BlendInvSrcAlpha,
	VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,  // BlendSrcAlphaSaturate,
	VK_BLEND_FACTOR_DST_COLOR,           // BlendDstColor,
	VK_BLEND_FACTOR_DST_ALPHA,           // BlendDstAlpha,
	VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, // BlendInvDstColor,
	VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA  // BlendInvDstAlpha
};

// Color factors are not used for the alpha channel, like in the Direct3D 11 backend
const VkBlendFactor arxToVkAlphaBlendFactor[] = {
	VK_BLEND_FACTOR_ZERO,                // BlendZero,
	VK_BLEND_FACTOR_ONE,                 // BlendOne,
	VK_BLEND_FACTOR_SRC_ALPHA,           // BlendSrcColor,
	VK_BLEND_FACTOR_SRC_ALPHA,           // BlendSrcAlpha,
	VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, // BlendInvSrcColor,
	VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, // BlendInvSrcAlpha,
	VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,  // BlendSrcAlphaSaturate,
	VK_BLEND_FACTOR_DST_ALPHA,           // BlendDstColor,
	VK_BLEND_FACTOR_DST_ALPHA,           // BlendDstAlpha,
	VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, // BlendInvDstColor,
	VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA  // BlendInvDstAlpha
};

const VkPrimitiveTopology arxToVkPrimitive[] = {
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,  // TriangleList,
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, // TriangleStrip,
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,   // TriangleFan,
	VK_PRIMITIVE_TOPOLOGY_LINE_LIST,      // LineList,
	VK_PRIMITIVE_TOPOLOGY_LINE_STRIP      // LineStrip
};

/*!
 * The pipeline state of a draw, packed into the key of the pipeline cache
 *
 * The viewport, the scissor rectangle and the depth bias values are dynamic state.
 */
struct PipelineState {

	unsigned vertexType = 0;
	VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	BlendingFactor blendSrc = BlendOne;
	BlendingFactor blendDst = BlendZero;
	bool alphaToCoverage = false;
	bool sampleShading = false;
	bool cull = false;
	bool depthTest = false;
	bool depthWrite = false;
	bool depthBias = false;
	bool wireframe = false;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

	uint64_t key() const {
		return uint64_t(vertexType) | (uint64_t(topology) << 2) | (uint64_t(blendSrc) << 5)
		       | (uint64_t(blendDst) << 9) | (uint64_t(alphaToCoverage) << 13) | (uint64_t(sampleShading) << 14)
		       | (uint64_t(cull) << 15) | (uint64_t(depthTest) << 16) | (uint64_t(depthWrite) << 17)
		       | (uint64_t(depthBias) << 18) | (uint64_t(wireframe) << 19) | (uint64_t(samples) << 20);
	}

	static PipelineState fromKey(uint64_t key) {
		PipelineState state;
		state.vertexType = unsigned(key & 3);
		state.topology = VkPrimitiveTopology((key >> 2) & 7);
		state.blendSrc = BlendingFactor((key >> 5) & 15);
		state.blendDst = BlendingFactor((key >> 9) & 15);
		state.alphaToCoverage = ((key >> 13) & 1) != 0;
		state.sampleShading = ((key >> 14) & 1) != 0;
		state.cull = ((key >> 15) & 1) != 0;
		state.depthTest = ((key >> 16) & 1) != 0;
		state.depthWrite = ((key >> 17) & 1) != 0;
		state.depthBias = ((key >> 18) & 1) != 0;
		state.wireframe = ((key >> 19) & 1) != 0;
		state.samples = VkSampleCountFlagBits((key >> 20) & 127);
		return state;
	}

};

VkShaderModule createShaderModule(VkDevice device, const unsigned char * code, size_t size, const char * name) {

	// The arrays generated by dxc are not aligned for uint32_t
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

fs::path pipelineCachePath() {
	const fs::path & userDir = fs::getUserDir();
	return userDir.empty() ? fs::path() : userDir / "vulkan-pipelines.bin";
}

} // anonymous namespace

bool VulkanRenderer::DescriptorKey::operator==(const DescriptorKey & o) const {
	return vsConstants == o.vsConstants && psConstants == o.psConstants
	       && std::equal(std::begin(views), std::end(views), std::begin(o.views))
	       && std::equal(std::begin(samplers), std::end(samplers), std::begin(o.samplers));
}

bool VulkanRenderer::createPipelineResources() {

	m_vertexShaders[VertexTL] = createShaderModule(m_device, spirv_vsTL, sizeof(spirv_vsTL), "vsTL");
	m_vertexShaders[VertexWorld] = createShaderModule(m_device, spirv_vsWorld, sizeof(spirv_vsWorld), "vsWorld");
	m_vertexShaders[VertexWorld3] = createShaderModule(m_device, spirv_vsWorld3, sizeof(spirv_vsWorld3), "vsWorld3");
	m_pixelShader = createShaderModule(m_device, spirv_psMain, sizeof(spirv_psMain), "psMain");
	if(!m_vertexShaders[VertexTL] || !m_vertexShaders[VertexWorld] || !m_vertexShaders[VertexWorld3]
	   || !m_pixelShader) {
		return false;
	}
	m_blitVertexShader = createShaderModule(m_device, spirv_vsBlit, sizeof(spirv_vsBlit), "vsBlit");
	m_blitPixelShader = createShaderModule(m_device, spirv_psBlit, sizeof(spirv_psBlit), "psBlit");
	if(!m_blitVertexShader || !m_blitPixelShader) {
		return false;
	}
	// Per-sample shading of alpha cutouts (SV_SampleIndex)
	if(m_features.sampleRateShading) {
		m_pixelShaderSampleShading = createShaderModule(m_device, spirv_psMainSampleShading,
		                                                sizeof(spirv_psMainSampleShading), "psMainSampleShading");
	}

	std::vector<VkDescriptorSetLayoutBinding> bindings;
	auto addBinding = [&bindings](uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages) {
		VkDescriptorSetLayoutBinding entry = {};
		entry.binding = binding;
		entry.descriptorType = type;
		entry.descriptorCount = 1;
		entry.stageFlags = stages;
		bindings.push_back(entry);
	};
	addBinding(VSConstantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_VERTEX_BIT);
	addBinding(PSConstantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_FRAGMENT_BIT);
	addBinding(LightConstantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);
	for(uint32_t i = 0; i < TextureSlotCount; i++) {
		addBinding(TextureBinding + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT);
	}
	for(uint32_t i = 0; i < TextureStageCount; i++) {
		addBinding(SamplerBinding + i, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
	}

	VkDescriptorSetLayoutCreateInfo setLayout = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	setLayout.bindingCount = uint32_t(bindings.size());
	setLayout.pBindings = bindings.data();
	if(vkCreateDescriptorSetLayout(m_device, &setLayout, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
		LogError << "Failed to create the Vulkan descriptor set layout";
		return false;
	}

	VkPipelineLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout.setLayoutCount = 1;
	layout.pSetLayouts = &m_descriptorSetLayout;
	if(vkCreatePipelineLayout(m_device, &layout, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
		LogError << "Failed to create the Vulkan pipeline layout";
		return false;
	}

	// Full-screen copies of the scene targets (vsBlit, psBlit)
	bindings.clear();
	addBinding(BlitConstantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_VERTEX_BIT);
	addBinding(TextureBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT);
	addBinding(SamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
	setLayout.bindingCount = uint32_t(bindings.size());
	setLayout.pBindings = bindings.data();
	if(vkCreateDescriptorSetLayout(m_device, &setLayout, nullptr, &m_blitSetLayout) != VK_SUCCESS) {
		LogError << "Failed to create the Vulkan blit descriptor set layout";
		return false;
	}
	layout.pSetLayouts = &m_blitSetLayout;
	if(vkCreatePipelineLayout(m_device, &layout, nullptr, &m_blitPipelineLayout) != VK_SUCCESS) {
		LogError << "Failed to create the Vulkan blit pipeline layout";
		return false;
	}

	// Pipelines from previous runs, the driver ignores data from other devices or driver versions
	std::string data;
	fs::path cachePath = pipelineCachePath();
	if(!cachePath.empty()) {
		data = fs::read(cachePath);
	}
	VkPipelineCacheCreateInfo cache = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	cache.initialDataSize = data.size();
	cache.pInitialData = data.empty() ? nullptr : data.data();
	if(vkCreatePipelineCache(m_device, &cache, nullptr, &m_pipelineCache) != VK_SUCCESS) {
		cache.initialDataSize = 0;
		cache.pInitialData = nullptr;
		if(vkCreatePipelineCache(m_device, &cache, nullptr, &m_pipelineCache) != VK_SUCCESS) {
			m_pipelineCache = VK_NULL_HANDLE;
		}
	}

	// No per-pixel lights yet, the shader still reads the light count
	{
		VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		info.size = sizeof(LightConstants);
		info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VmaAllocationCreateInfo allocation = {};
		allocation.usage = VMA_MEMORY_USAGE_AUTO;
		allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo allocationInfo = {};
		if(vmaCreateBuffer(m_allocator, &info, &allocation, &m_lightConstants, &m_lightConstantsAllocation,
		                   &allocationInfo) != VK_SUCCESS) {
			LogError << "Failed to create the Vulkan light constants";
			return false;
		}
		std::memset(allocationInfo.pMappedData, 0, sizeof(LightConstants));
		vmaFlushAllocation(m_allocator, m_lightConstantsAllocation, 0, VK_WHOLE_SIZE);
	}

	// Texture slots without a texture still need a valid image
	{
		Image image;
		image.create(1, 1, Image::Format_R8G8B8A8);
		std::memset(image.getData(), 255, 4);
		m_whiteTexture = std::make_unique<VulkanTexture>(this);
		if(!m_whiteTexture->Texture::create(image, 0) || !m_whiteTexture->view()) {
			LogError << "Failed to create the Vulkan default texture";
			return false;
		}
	}

	return true;
}

void VulkanRenderer::releasePipelineResources() {

	if(m_pipelineCache) {
		size_t size = 0;
		fs::path cachePath = pipelineCachePath();
		if(!cachePath.empty() && vkGetPipelineCacheData(m_device, m_pipelineCache, &size, nullptr) == VK_SUCCESS
		   && size > 0) {
			std::string data(size, '\0');
			if(vkGetPipelineCacheData(m_device, m_pipelineCache, &size, data.data()) == VK_SUCCESS) {
				data.resize(size);
				fs::write(cachePath, data);
			}
		}
		vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
		m_pipelineCache = VK_NULL_HANDLE;
	}

	for(const auto & entry : m_pipelines) {
		vkDestroyPipeline(m_device, entry.second, nullptr);
	}
	m_pipelines.clear();

	vkDestroyPipeline(m_device, m_blitPipeline, nullptr);
	m_blitPipeline = VK_NULL_HANDLE;
	vkDestroyPipelineLayout(m_device, m_blitPipelineLayout, nullptr);
	m_blitPipelineLayout = VK_NULL_HANDLE;
	vkDestroyDescriptorSetLayout(m_device, m_blitSetLayout, nullptr);
	m_blitSetLayout = VK_NULL_HANDLE;
	vkDestroyShaderModule(m_device, m_blitVertexShader, nullptr);
	m_blitVertexShader = VK_NULL_HANDLE;
	vkDestroyShaderModule(m_device, m_blitPixelShader, nullptr);
	m_blitPixelShader = VK_NULL_HANDLE;

	if(m_whiteTexture) {
		m_whiteTexture->releaseImage();
		m_whiteTexture->m_renderer = nullptr;
		m_whiteTexture.reset();
	}

	if(m_lightConstants) {
		vmaDestroyBuffer(m_allocator, m_lightConstants, m_lightConstantsAllocation);
		m_lightConstants = VK_NULL_HANDLE;
	}

	vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
	m_pipelineLayout = VK_NULL_HANDLE;
	vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
	m_descriptorSetLayout = VK_NULL_HANDLE;

	for(VkShaderModule & module : m_vertexShaders) {
		vkDestroyShaderModule(m_device, module, nullptr);
		module = VK_NULL_HANDLE;
	}
	vkDestroyShaderModule(m_device, m_pixelShader, nullptr);
	m_pixelShader = VK_NULL_HANDLE;
	vkDestroyShaderModule(m_device, m_pixelShaderSampleShading, nullptr);
	m_pixelShaderSampleShading = VK_NULL_HANDLE;

}

VkPipeline VulkanRenderer::getPipeline(uint64_t key) {

	auto it = m_pipelines.find(key);
	if(it != m_pipelines.end()) {
		return it->second;
	}

	PipelineState state = PipelineState::fromKey(key);

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }
	};
	static const char * const vertexEntries[VertexTypeCount] = { "vsTL", "vsWorld", "vsWorld3" };
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = m_vertexShaders[state.vertexType];
	stages[0].pName = vertexEntries[state.vertexType];
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = state.sampleShading ? m_pixelShaderSampleShading : m_pixelShader;
	stages[1].pName = "psMain";

	VkVertexInputBindingDescription binding = {};
	binding.binding = 0;
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	std::vector<VkVertexInputAttributeDescription> attributes;
	auto addAttribute = [&attributes](uint32_t location, VkFormat format, size_t offset) {
		attributes.push_back({ location, 0, format, uint32_t(offset) });
	};
	switch(state.vertexType) {
		case VertexTL: {
			binding.stride = sizeof(TexturedVertex);
			static_assert(offsetof(TexturedVertex, w) == offsetof(TexturedVertex, p) + 3 * sizeof(float),
			              "TexturedVertex position must be followed by w");
			addAttribute(0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TexturedVertex, p));
			addAttribute(1, VK_FORMAT_R8G8B8A8_UNORM, offsetof(TexturedVertex, color));
			addAttribute(2, VK_FORMAT_R32G32_SFLOAT, offsetof(TexturedVertex, uv));
			addAttribute(3, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TexturedVertex, prevP));
			addAttribute(4, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TexturedVertex, normal));
			break;
		}
		case VertexWorld: {
			binding.stride = sizeof(SMY_VERTEX);
			addAttribute(0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SMY_VERTEX, p));
			addAttribute(1, VK_FORMAT_R8G8B8A8_UNORM, offsetof(SMY_VERTEX, color));
			addAttribute(2, VK_FORMAT_R32G32_SFLOAT, offsetof(SMY_VERTEX, uv));
			addAttribute(3, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SMY_VERTEX, normal));
			break;
		}
		case VertexWorld3: {
			binding.stride = sizeof(SMY_VERTEX3);
			addAttribute(0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SMY_VERTEX3, p));
			addAttribute(1, VK_FORMAT_R8G8B8A8_UNORM, offsetof(SMY_VERTEX3, color));
			addAttribute(2, VK_FORMAT_R32G32_SFLOAT, offsetof(SMY_VERTEX3, uv));
			addAttribute(3, VK_FORMAT_R32G32_SFLOAT, offsetof(SMY_VERTEX3, uv) + sizeof(Vec2f));
			addAttribute(4, VK_FORMAT_R32G32_SFLOAT, offsetof(SMY_VERTEX3, uv) + 2 * sizeof(Vec2f));
			break;
		}
		default: arx_unreachable();
	}

	VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertexInput.vertexBindingDescriptionCount = 1;
	vertexInput.pVertexBindingDescriptions = &binding;
	vertexInput.vertexAttributeDescriptionCount = uint32_t(attributes.size());
	vertexInput.pVertexAttributeDescriptions = attributes.data();

	VkPipelineInputAssemblyStateCreateInfo inputAssembly
		= { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	inputAssembly.topology = state.topology;

	VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterization
		= { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rasterization.polygonMode = state.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
	rasterization.cullMode = state.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
	// Same winding as the OpenGL default, the viewport flips the y axis like OpenGL and Direct3D
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.depthBiasEnable = state.depthBias ? VK_TRUE : VK_FALSE;
	rasterization.lineWidth = 1.f;

	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = state.samples;
	multisample.alphaToCoverageEnable = state.alphaToCoverage ? VK_TRUE : VK_FALSE;

	// Like OpenGL, the depth test is always enabled so that depth writes work
	VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = state.depthWrite ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp = state.depthTest ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_ALWAYS;

	VkPipelineColorBlendAttachmentState blend = {};
	blend.blendEnable = (state.blendSrc != BlendOne || state.blendDst != BlendZero) ? VK_TRUE : VK_FALSE;
	blend.srcColorBlendFactor = arxToVkBlendFactor[state.blendSrc];
	blend.dstColorBlendFactor = arxToVkBlendFactor[state.blendDst];
	blend.colorBlendOp = VK_BLEND_OP_ADD;
	blend.srcAlphaBlendFactor = arxToVkAlphaBlendFactor[state.blendSrc];
	blend.dstAlphaBlendFactor = arxToVkAlphaBlendFactor[state.blendDst];
	blend.alphaBlendOp = VK_BLEND_OP_ADD;
	blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
	                       | VK_COLOR_COMPONENT_A_BIT;

	// The scene targets written by psMain (SV_Target1-3) don't exist yet, see beginRendering()
	const VkFormat formats[] = { m_colorFormat, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
	// Identical states do not need the independentBlend feature
	const VkPipelineColorBlendAttachmentState blends[] = { blend, blend, blend, blend };

	VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	colorBlend.attachmentCount = uint32_t(std::size(blends));
	colorBlend.pAttachments = blends;

	const VkDynamicState dynamicStates[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
		VK_DYNAMIC_STATE_DEPTH_BIAS,
	};
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.dynamicStateCount = uint32_t(std::size(dynamicStates));
	dynamic.pDynamicStates = dynamicStates;

	VkPipelineRenderingCreateInfoKHR rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	rendering.colorAttachmentCount = uint32_t(std::size(formats));
	rendering.pColorAttachmentFormats = formats;
	rendering.depthAttachmentFormat = m_depthFormat;
	rendering.stencilAttachmentFormat = m_depthFormat;

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
	info.layout = m_pipelineLayout;

	VkPipeline pipeline = VK_NULL_HANDLE;
	if(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
		LogError << "Failed to create Vulkan pipeline " << key;
		pipeline = VK_NULL_HANDLE;
	}

	// Also remember failures so that they are not retried for every draw
	m_pipelines.emplace(key, pipeline);
	return pipeline;
}

glm::mat4x4 VulkanRenderer::screenTransform() const {

	// Change coordinate system from [0, width] x [0, height] to [-1, 1] x [-1, 1] and flip the y axis
	glm::mat4x4 transform = glm::translate(glm::mat4x4(1.f), Vec3f(-1.f, 1.f, 0.f));
	transform = glm::scale(transform, Vec3f(2.f / float(std::max<s32>(m_viewport.width(), 1)),
	                                        -2.f / float(std::max<s32>(m_viewport.height(), 1)), 1.f));
	// Change pixel origins
	return glm::translate(transform, Vec3f(0.5f, 0.5f, 0.f));
}

bool VulkanRenderer::prepareDraw(VertexType type, Primitive primitive) {

	if(!m_colorTarget.image || m_deviceLost || !m_pipelineLayout) {
		return false;
	}

	beginRendering();

	VkCommandBuffer commands = m_frames[m_frameIndex].commands;

	// Blending and alpha cutouts - same logic as in the OpenGL and Direct3D 11 backends

	enum TestMode {
		TestSS,
		TestA2C,
		TestStrict,
		TestConservative,
		TestNone,
	};

	bool useSS = m_hasMSAA && m_pixelShaderSampleShading
	             && config.video.alphaCutoutAntialiasing == int(CrispAlphaCutoutAA);
	bool useA2C = m_hasMSAA && config.video.alphaCutoutAntialiasing == int(FuzzyAlphaCutoutAA);

	/* When rendering alpha cutouts with alpha blending enabled we still
	 * need to 'discard' transparent texels, as blending might not use the src alpha!
	 * On the other hand, we can't use alpha to coverage when blending
	 * as that could result in the src alpha being applied twice (e.g. for text).
	 * So we must toggle between alpha to coverage and alpha test when toggling blending.
	 */
	BlendingFactor blendSrc = m_state.getBlendSrc();
	BlendingFactor blendDst = m_state.getBlendDst();
	TestMode alphaTest = TestNone;
	if(m_state.getAlphaCutout()) {
		arx_assert_msg(blendSrc != BlendInvSrcAlpha && blendDst != BlendSrcAlpha,
		               "inverted alpha blending combined with alpha test makes no sense");
		if(blendSrc == BlendOne && blendDst == BlendZero) {
			alphaTest = useSS ? TestSS : useA2C ? TestA2C : TestStrict;
		} else if(blendSrc == BlendOne && blendDst == BlendOne) {
			blendSrc = BlendSrcAlpha;
			alphaTest = TestConservative; // optimization only, could use TestNone
		} else if(blendSrc == BlendSrcAlpha || blendDst == BlendInvSrcAlpha) {
			alphaTest = TestConservative;
		} else {
			alphaTest = useSS ? TestSS : TestStrict;
		}
	}

	float alphaReference = -1.f;
	if(alphaTest == TestConservative || alphaTest == TestA2C) {
		alphaReference = 0.f;
	} else if(alphaTest == TestStrict || alphaTest == TestSS) {
		alphaReference = 0.5f;
	}

	// Pipeline

	PipelineState state;
	state.vertexType = unsigned(type);
	state.topology = arxToVkPrimitive[primitive];
	state.blendSrc = blendSrc;
	state.blendDst = blendDst;
	state.alphaToCoverage = (alphaTest == TestA2C);
	state.sampleShading = (alphaTest == TestSS);
	state.cull = m_state.getCull();
	state.depthTest = m_state.getDepthTest();
	state.depthWrite = m_state.getDepthWrite();
	state.depthBias = (m_state.getDepthOffset() != 0);
	state.wireframe = m_wireframe;
	state.samples = m_msaaSamples;

	VkPipeline pipeline = getPipeline(state.key());
	if(!pipeline) {
		return false;
	}
	if(pipeline != m_boundPipeline) {
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		m_boundPipeline = pipeline;
	}

	// Dynamic state

	if(!m_dynamicStateValid) {

		// The game uses window pixels, the scaled 3D scene is smaller
		Vec2i size = m_inScene ? m_sceneSize : m_size;
		Vec2f scale = targetScale();
		Rect bounds(size.x, size.y);

		// Negative height: the y axis points up in clip space like in OpenGL and Direct3D
		VkViewport viewport;
		viewport.x = float(m_viewport.left) * scale.x;
		viewport.y = float(m_viewport.bottom) * scale.y;
		viewport.width = float(std::max<s32>(m_viewport.width(), 1)) * scale.x;
		viewport.height = -float(std::max<s32>(m_viewport.height(), 1)) * scale.y;
		viewport.minDepth = 0.f;
		viewport.maxDepth = 1.f;
		vkCmdSetViewport(commands, 0, 1, &viewport);

		Rect scissor = m_scissor.isValid() ? (toTargetRect(m_scissor) & bounds) : bounds;
		VkRect2D rect = { { 0, 0 }, { 0, 0 } };
		if(scissor.isValid() && scissor.width() > 0 && scissor.height() > 0) {
			rect = { { scissor.left, scissor.top }, { uint32_t(scissor.width()), uint32_t(scissor.height()) } };
		}
		vkCmdSetScissor(commands, 0, 1, &rect);

		m_dynamicStateValid = true;
	}

	if(state.depthBias) {
		float bias = -float(m_state.getDepthOffset());
		if(bias != m_boundDepthBias) {
			vkCmdSetDepthBias(commands, bias, 0.f, bias);
			m_boundDepthBias = bias;
		}
	}

	// Constants

	VkDeviceSize alignment = std::max<VkDeviceSize>(m_properties.limits.minUniformBufferOffsetAlignment, 16);

	{
		VSConstants constants;
		glm::mat4x4 transform = (type == VertexTL) ? screenTransform() : m_projection * m_view;
		std::memcpy(constants.transform, &transform, sizeof(constants.transform));
		std::memcpy(constants.prevTransform, &transform, sizeof(constants.prevTransform));
		std::fill(std::begin(constants.jitter), std::end(constants.jitter), 0.f);
		// The view matrix is a rotation and translation
		Vec3f camera = -(glm::transpose(glm::mat3(m_view)) * Vec3f(m_view[3]));
		constants.camera[0] = camera.x;
		constants.camera[1] = camera.y;
		constants.camera[2] = camera.z;
		constants.camera[3] = 0.f;
		glm::mat4x4 modelToWorld(1.f);
		std::memcpy(constants.modelToWorld, &modelToWorld, sizeof(constants.modelToWorld));
		glm::mat4x4 worldToClip = m_projection * m_view;
		std::memcpy(constants.worldToClip, &worldToClip, sizeof(constants.worldToClip));

		if(!m_vsConstants.buffer || m_vsConstantsData.size() != sizeof(constants)
		   || std::memcmp(m_vsConstantsData.data(), &constants, sizeof(constants)) != 0) {
			if(!allocate(m_frames[m_frameIndex].constants, sizeof(constants), alignment, m_vsConstants)) {
				return false;
			}
			std::memcpy(m_vsConstants.data, &constants, sizeof(constants));
			m_vsConstantsData.assign(reinterpret_cast<const u8 *>(&constants),
			                         reinterpret_cast<const u8 *>(&constants) + sizeof(constants));
		}
	}

	DescriptorKey descriptors;

	{
		PSConstants constants = {};

		for(size_t i = 0; i < TextureStageCount; i++) {
			VulkanTextureStage * stage = GetTextureStage(i);
			VulkanTexture * texture = stage->getActiveTexture();
			descriptors.views[i] = texture ? texture->view() : m_whiteTexture->view();
			descriptors.samplers[i] = texture ? stage->getSampler(texture) : stage->getSampler(m_whiteTexture.get());
			constants.stages[i][0] = s32(stage->getColorOp());
			constants.stages[i][1] = s32(stage->getAlphaOp());
			constants.stages[i][2] = texture ? 1 : 0;
			constants.stages[i][3] = 0; // No relief map yet
		}
		descriptors.views[3] = m_whiteTexture->view();

		constants.fogColor[0] = m_fogColor.r;
		constants.fogColor[1] = m_fogColor.g;
		constants.fogColor[2] = m_fogColor.b;
		constants.fogColor[3] = m_fogColor.a;
		constants.fogParams[0] = m_fogStart;
		constants.fogParams[1] = m_fogEnd;
		constants.fogParams[2] = m_state.getFog() ? 1.f : 0.f;
		constants.fogParams[3] = alphaReference;
		constants.surface[1] = m_state.isBlendEnabled() ? 1.f : 0.f;
		// Models are already lit in their vertex colors and only get highlights
		constants.surface[3] = (type == VertexTL) ? 1.f : 0.f;

		if(!m_psConstants.buffer || m_psConstantsData.size() != sizeof(constants)
		   || std::memcmp(m_psConstantsData.data(), &constants, sizeof(constants)) != 0) {
			if(!allocate(m_frames[m_frameIndex].constants, sizeof(constants), alignment, m_psConstants)) {
				return false;
			}
			std::memcpy(m_psConstants.data, &constants, sizeof(constants));
			m_psConstantsData.assign(reinterpret_cast<const u8 *>(&constants),
			                         reinterpret_cast<const u8 *>(&constants) + sizeof(constants));
		}
	}

	// Descriptors

	descriptors.vsConstants = m_vsConstants.buffer;
	descriptors.psConstants = m_psConstants.buffer;

	bool setChanged = false;
	if(!m_boundDescriptorSet || !(descriptors == m_boundDescriptors)) {

		VkDescriptorSet set = allocateDescriptorSet(m_descriptorSetLayout);
		if(!set) {
			return false;
		}

		VkDescriptorBufferInfo vsBuffer = { descriptors.vsConstants, 0, sizeof(VSConstants) };
		VkDescriptorBufferInfo psBuffer = { descriptors.psConstants, 0, sizeof(PSConstants) };
		VkDescriptorBufferInfo lightBuffer = { m_lightConstants, 0, sizeof(LightConstants) };
		VkDescriptorImageInfo images[TextureSlotCount];
		for(size_t i = 0; i < TextureSlotCount; i++) {
			images[i] = { VK_NULL_HANDLE, descriptors.views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		}
		VkDescriptorImageInfo samplers[TextureStageCount];
		for(size_t i = 0; i < TextureStageCount; i++) {
			samplers[i] = { descriptors.samplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
		}

		VkWriteDescriptorSet writes[5];
		for(VkWriteDescriptorSet & write : writes) {
			write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			write.dstSet = set;
			write.descriptorCount = 1;
		}
		writes[0].dstBinding = VSConstantsBinding;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		writes[0].pBufferInfo = &vsBuffer;
		writes[1].dstBinding = PSConstantsBinding;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		writes[1].pBufferInfo = &psBuffer;
		writes[2].dstBinding = LightConstantsBinding;
		writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[2].pBufferInfo = &lightBuffer;
		// Consecutive bindings of the same type can be written as an array
		writes[3].dstBinding = TextureBinding;
		writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[3].descriptorCount = uint32_t(TextureSlotCount);
		writes[3].pImageInfo = images;
		writes[4].dstBinding = SamplerBinding;
		writes[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		writes[4].descriptorCount = uint32_t(TextureStageCount);
		writes[4].pImageInfo = samplers;
		vkUpdateDescriptorSets(m_device, uint32_t(std::size(writes)), writes, 0, nullptr);

		m_boundDescriptorSet = set;
		m_boundDescriptors = descriptors;
		setChanged = true;
	}

	if(setChanged || m_boundConstantOffsets[0] != uint32_t(m_vsConstants.offset)
	   || m_boundConstantOffsets[1] != uint32_t(m_psConstants.offset)) {
		m_boundConstantOffsets[0] = uint32_t(m_vsConstants.offset);
		m_boundConstantOffsets[1] = uint32_t(m_psConstants.offset);
		vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1,
		                        &m_boundDescriptorSet, 2, m_boundConstantOffsets);
	}

	return true;
}

VkPipeline VulkanRenderer::getBlitPipeline() {
	
	if(m_blitPipeline) {
		return m_blitPipeline;
	}
	
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }
	};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = m_blitVertexShader;
	stages[0].pName = "vsBlit";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = m_blitPixelShader;
	stages[1].pName = "psBlit";
	
	// A full-screen triangle from SV_VertexID
	VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo inputAssembly
		= { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	
	VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	
	VkPipelineRasterizationStateCreateInfo rasterization
		= { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.f;
	
	// The window target
	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = m_msaaSamples;
	
	VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	
	VkPipelineColorBlendAttachmentState blend = {};
	blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
	                       | VK_COLOR_COMPONENT_A_BIT;
	// Same attachments as the other pipelines, see beginRendering()
	const VkFormat formats[] = { m_colorFormat, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
	const VkPipelineColorBlendAttachmentState blends[] = { blend, blend, blend, blend };
	VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	colorBlend.attachmentCount = uint32_t(std::size(blends));
	colorBlend.pAttachments = blends;
	
	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.dynamicStateCount = uint32_t(std::size(dynamicStates));
	dynamic.pDynamicStates = dynamicStates;
	
	VkPipelineRenderingCreateInfoKHR rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	rendering.colorAttachmentCount = uint32_t(std::size(formats));
	rendering.pColorAttachmentFormats = formats;
	rendering.depthAttachmentFormat = m_depthFormat;
	rendering.stencilAttachmentFormat = m_depthFormat;
	
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
	info.layout = m_blitPipelineLayout;
	
	if(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &info, nullptr, &m_blitPipeline) != VK_SUCCESS) {
		LogError << "Failed to create the Vulkan blit pipeline";
		m_blitPipeline = VK_NULL_HANDLE;
	}
	
	return m_blitPipeline;
}

void VulkanRenderer::composeScene(VkImageView scene) {
	
	beginRendering();
	if(!m_renderingActive) {
		return;
	}
	
	VkPipeline pipeline = getBlitPipeline();
	VkDescriptorSet set = pipeline ? allocateDescriptorSet(m_blitSetLayout) : VK_NULL_HANDLE;
	RingAllocation constants;
	VkDeviceSize alignment = std::max<VkDeviceSize>(m_properties.limits.minUniformBufferOffsetAlignment, 16);
	if(!set || !allocate(m_frames[m_frameIndex].constants, sizeof(BlitConstants), alignment, constants)) {
		return;
	}
	BlitConstants blit = { { 0.f, 0.f, 1.f, 1.f } };
	std::memcpy(constants.data, &blit, sizeof(blit));
	
	VkDescriptorBufferInfo buffer = { constants.buffer, 0, sizeof(BlitConstants) };
	VkDescriptorImageInfo image = { VK_NULL_HANDLE, scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo sampler = {
		getSampler(TextureStage::WrapClamp, TextureStage::FilterLinear, TextureStage::FilterLinear, false, 0.f),
		VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED
	};
	VkWriteDescriptorSet writes[3];
	for(VkWriteDescriptorSet & write : writes) {
		write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet = set;
		write.descriptorCount = 1;
	}
	writes[0].dstBinding = BlitConstantsBinding;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	writes[0].pBufferInfo = &buffer;
	writes[1].dstBinding = TextureBinding;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	writes[1].pImageInfo = &image;
	writes[2].dstBinding = SamplerBinding;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	writes[2].pImageInfo = &sampler;
	vkUpdateDescriptorSets(m_device, uint32_t(std::size(writes)), writes, 0, nullptr);
	
	VkCommandBuffer commands = m_frames[m_frameIndex].commands;
	
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	uint32_t offset = uint32_t(constants.offset);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blitPipelineLayout, 0, 1, &set, 1, &offset);
	
	VkViewport viewport = { 0.f, float(m_size.y), float(m_size.x), -float(m_size.y), 0.f, 1.f };
	vkCmdSetViewport(commands, 0, 1, &viewport);
	VkRect2D scissor = { { 0, 0 }, { uint32_t(m_size.x), uint32_t(m_size.y) } };
	vkCmdSetScissor(commands, 0, 1, &scissor);
	
	vkCmdDraw(commands, 3, 1, 0, 0);
	
	// The next draw binds its own state
	invalidateBoundState();
	
}
