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

#include <glm/gtc/constants.hpp>
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
#include "psNormalMap.h"

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

// Same as the Direct3D 11 backend

// Normal map slope at full strength, the generated slope is the brightness change per texel
const float NormalMapSlopeScale = 6.f;

// Parallax depth at full strength, relative to the texture size
const float ParallaxDepthScale = 0.05f;

// Highlight strength of the shiniest materials at full strength
const float SpecularScale = 1.5f;

// Highlights and reflections of water surfaces
const TextureMaterial WaterMaterial = { 1.f, 96.f, 0.f, 1.f };

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
	bool premultipliedAlpha = false; //!< The VR interface layer, see setPremultipliedAlphaOutput()
	bool stencilMark = false; //!< Set the scene stencil to 1: no effects there, see setSceneEffectsExcluded()

	uint64_t key() const {
		return uint64_t(vertexType) | (uint64_t(topology) << 2) | (uint64_t(blendSrc) << 5)
		       | (uint64_t(blendDst) << 9) | (uint64_t(alphaToCoverage) << 13) | (uint64_t(sampleShading) << 14)
		       | (uint64_t(cull) << 15) | (uint64_t(depthTest) << 16) | (uint64_t(depthWrite) << 17)
		       | (uint64_t(depthBias) << 18) | (uint64_t(wireframe) << 19) | (uint64_t(samples) << 20)
		       | (uint64_t(premultipliedAlpha) << 27) | (uint64_t(stencilMark) << 28);
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
		state.premultipliedAlpha = ((key >> 27) & 1) != 0;
		state.stencilMark = ((key >> 28) & 1) != 0;
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
	// Relief maps for normal and parallax mapping are optional
	m_reliefPixelShader = createShaderModule(m_device, spirv_psNormalMap, sizeof(spirv_psNormalMap), "psNormalMap");
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
	// The lights change during the frame, they are copied into the memory of the frame like the other constants
	addBinding(LightConstantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_FRAGMENT_BIT);
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

	// Full-screen passes outside of frames (relief maps) read the whole source
	{
		VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		info.size = sizeof(BlitConstants);
		info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VmaAllocationCreateInfo allocation = {};
		allocation.usage = VMA_MEMORY_USAGE_AUTO;
		allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo allocationInfo = {};
		if(vmaCreateBuffer(m_allocator, &info, &allocation, &m_fullRectConstants, &m_fullRectAllocation,
		                   &allocationInfo) != VK_SUCCESS) {
			LogError << "Failed to create the Vulkan blit constants";
			return false;
		}
		BlitConstants constants = { { 0.f, 0.f, 1.f, 1.f } };
		std::memcpy(allocationInfo.pMappedData, &constants, sizeof(constants));
		vmaFlushAllocation(m_allocator, m_fullRectAllocation, 0, VK_WHOLE_SIZE);
	}

	// No per-pixel lights until setPixelLights(), the shader still reads the light count
	m_lightData.assign(sizeof(LightConstants), 0);
	m_lightDataSize = sizeof(LightConstants::count);
	m_lights = FrameCopy();

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

	vkDestroyShaderModule(m_device, m_blitVertexShader, nullptr);
	m_blitVertexShader = VK_NULL_HANDLE;
	vkDestroyShaderModule(m_device, m_blitPixelShader, nullptr);
	m_blitPixelShader = VK_NULL_HANDLE;

	if(m_whiteTexture) {
		m_whiteTexture->releaseImage();
		m_whiteTexture->m_renderer = nullptr;
		m_whiteTexture.reset();
	}

	if(m_fullRectConstants) {
		vmaDestroyBuffer(m_allocator, m_fullRectConstants, m_fullRectAllocation);
		m_fullRectConstants = VK_NULL_HANDLE;
	}

	vkDestroyPipeline(m_device, m_reliefPipeline, nullptr);
	m_reliefPipeline = VK_NULL_HANDLE;
	vkDestroyShaderModule(m_device, m_reliefPixelShader, nullptr);
	m_reliefPixelShader = VK_NULL_HANDLE;

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
	if(state.stencilMark) {
		// Like the Direct3D 11 backend: the pixels that pass the depth test are marked
		depthStencil.stencilTestEnable = VK_TRUE;
		VkStencilOpState mark = {};
		mark.failOp = mark.depthFailOp = VK_STENCIL_OP_KEEP;
		mark.passOp = VK_STENCIL_OP_REPLACE;
		mark.compareOp = VK_COMPARE_OP_ALWAYS;
		mark.compareMask = mark.writeMask = 0xff;
		mark.reference = 1;
		depthStencil.front = depthStencil.back = mark;
	}

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
	if(state.premultipliedAlpha) {
		/*
		 * Produce a premultiplied alpha image that can be composited over another one, like the other backends:
		 * the color channels are blended as usual while the alpha channel accumulates coverage.
		 * Additive and multiplicative blending do not add coverage.
		 */
		bool coverage = (state.blendDst != BlendOne && state.blendSrc != BlendZero);
		blend.blendEnable = VK_TRUE;
		blend.srcAlphaBlendFactor = coverage ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
		blend.dstAlphaBlendFactor = coverage ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
	}

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

	// The scene has its own sample count in VR, the window none
	VkSampleCountFlagBits samples = m_inScene ? m_sceneSamples : m_msaaSamples;
	bool hasMSAA = m_hasMSAA && samples != VK_SAMPLE_COUNT_1_BIT;
	bool useSS = hasMSAA && m_pixelShaderSampleShading
	             && config.video.alphaCutoutAntialiasing == int(CrispAlphaCutoutAA);
	bool useA2C = hasMSAA && config.video.alphaCutoutAntialiasing == int(FuzzyAlphaCutoutAA);

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
	state.samples = samples;
	state.premultipliedAlpha = m_premultipliedAlpha && !m_inScene;
	state.stencilMark = m_sceneEffectsExcluded && m_inScene && m_sceneStencilView;

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

		// The game uses window pixels, the scaled 3D scene is smaller (see drawViews for the stereo scene)
		Vec2i size = targetSize();
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

		// Screen-space scissor rectangles do not map to the stereo views
		Rect scissor = (m_scissor.isValid() && !stereoActive()) ? (toTargetRect(m_scissor) & bounds) : bounds;
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
		// Pre-transformed vertices of models in the 3D scene go back to world space with the scene camera (the
		// center camera in VR, which projected them), for the per-pixel highlights. World coordinates are large,
		// so this is computed in double precision.
		glm::mat4x4 modelToWorld(1.f);
		if(type == VertexTL && m_drawingScene) {
			modelToWorld = glm::mat4x4(glm::inverse(glm::dmat4(m_projection) * glm::dmat4(m_view))
			                           * glm::dmat4(screenTransform()));
		}

		auto fill = [&](VSConstants & constants, const glm::mat4x4 & transform, const glm::mat4x4 & view,
		                const glm::mat4x4 & projection) {
			std::memcpy(constants.transform, &transform, sizeof(constants.transform));
			std::memcpy(constants.prevTransform, &transform, sizeof(constants.prevTransform));
			std::fill(std::begin(constants.jitter), std::end(constants.jitter), 0.f);
			// The view matrix is a rotation and translation
			Vec3f camera = -(glm::transpose(glm::mat3(view)) * Vec3f(view[3]));
			constants.camera[0] = camera.x;
			constants.camera[1] = camera.y;
			constants.camera[2] = camera.z;
			constants.camera[3] = 0.f;
			std::memcpy(constants.modelToWorld, &modelToWorld, sizeof(constants.modelToWorld));
			glm::mat4x4 worldToClip = projection * view;
			std::memcpy(constants.worldToClip, &worldToClip, sizeof(constants.worldToClip));
		};

		// The eyes of the stereo scene have their own constants, both in one allocation, see drawViews()
		const size_t MaxViews = 2; // See m_stereoVSOffsets
		size_t views = stereoActive() ? std::min(m_stereoViews.size(), MaxViews) : 1;
		VSConstants constants[MaxViews];
		if(stereoActive()) {
			for(size_t eye = 0; eye < views; eye++) {
				const StereoView & view = m_stereoViews[eye];
				// Pre-transformed vertices are re-projected from the center camera into the eye
				glm::mat4x4 transform = (type == VertexTL) ? view.screenToClip : view.projection * view.view;
				fill(constants[eye], transform, view.view, view.projection);
			}
		} else {
			glm::mat4x4 transform = (type == VertexTL) ? screenTransform() : m_projection * m_view;
			fill(constants[0], transform, m_view, m_projection);
		}
		size_t size = views * sizeof(VSConstants);

		if(!m_vsConstants.buffer || m_vsConstantsData.size() != size
		   || std::memcmp(m_vsConstantsData.data(), constants, size) != 0) {
			VkDeviceSize stride = (sizeof(VSConstants) + alignment - 1) / alignment * alignment;
			if(!allocate(m_frames[m_frameIndex].constants, stride * views, alignment, m_vsConstants)) {
				return false;
			}
			for(size_t eye = 0; eye < views; eye++) {
				std::memcpy(static_cast<u8 *>(m_vsConstants.data) + eye * stride, &constants[eye], sizeof(VSConstants));
				m_stereoVSOffsets[eye] = uint32_t(m_vsConstants.offset + eye * stride);
			}
			m_vsConstantsData.assign(reinterpret_cast<const u8 *>(constants),
			                         reinterpret_cast<const u8 *>(constants) + size);
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
		}
		descriptors.views[3] = m_whiteTexture->view();

		// Material of the main texture, same as the Direct3D 11 backend (without the model tessellation)
		if(VulkanTexture * texture = GetTextureStage(0)->getActiveTexture()) {

			const TextureMaterial & material = texture->material();

			// Relief map for normal mapping and parallax mapping of world geometry
			if(type == VertexWorld && reliefMapsEnabled() && texture->reliefView()) {
				descriptors.views[3] = texture->reliefView();
				constants.stages[0][3] = 1;
				constants.relief[0] = m_normalMapStrength * NormalMapSlopeScale;
				constants.relief[1] = m_parallaxStrength * ParallaxDepthScale;
			}

			// Highlights from the material of the main texture
			if((type == VertexWorld || (type == VertexTL && m_drawingScene)) && m_specularStrength > 0.f) {
				constants.specular[0] = material.strength * m_specularStrength * SpecularScale;
				constants.specular[1] = material.exponent;
				constants.specular[2] = material.metallic;
				constants.specular[3] = m_specularStrength * SpecularScale;
				// Metal and leather parts of armor in the 3D scene
				if(type == VertexTL) {
					constants.material[0] = material.metalParts;
					constants.material[1] = material.leather;
				}
			}

			// Lava glows and flows in the 3D scene
			if((type == VertexWorld || type == VertexTL) && m_drawingScene && material.glow > 0.f) {
				constants.effect[0] = m_effectTime;
				constants.effect[1] = material.glow;
			}
			// Ice looks deep, cold and sparkles in the 3D scene, crystals and gems (also items) look clear and deep
			if(type == VertexWorld && m_drawingScene) {
				constants.effect[2] = material.ice;
			}
			if((type == VertexWorld || type == VertexTL) && m_drawingScene) {
				constants.effect[3] = material.crystal;
			}

		}

		// Water is drawn without normals, with an animated texture
		if(type == VertexWorld3 && m_waterSurface) {
			constants.specular[0] = WaterMaterial.strength * m_specularStrength * SpecularScale;
			constants.specular[1] = WaterMaterial.exponent;
			constants.specular[2] = WaterMaterial.metallic;
			constants.surface[2] = 1.f;
		}

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

	// The lights of the frame, copied once per frame and again when they change
	if(m_lights.frame != m_frameSerial || !m_lights.buffer) {
		RingAllocation lights;
		if(!allocate(m_frames[m_frameIndex].constants, sizeof(LightConstants), alignment, lights)) {
			return false;
		}
		// Only the lights in use are read
		std::memcpy(lights.data, m_lightData.data(), m_lightDataSize);
		m_lights.frame = m_frameSerial;
		m_lights.buffer = lights.buffer;
		m_lights.offset = lights.offset;
	}

	// Descriptors

	descriptors.vsConstants = m_vsConstants.buffer;
	descriptors.psConstants = m_psConstants.buffer;
	descriptors.lightConstants = m_lights.buffer;

	bool setChanged = false;
	if(!m_boundDescriptorSet || !(descriptors == m_boundDescriptors)) {

		VkDescriptorSet set = allocateDescriptorSet(m_descriptorSetLayout);
		if(!set) {
			return false;
		}

		VkDescriptorBufferInfo vsBuffer = { descriptors.vsConstants, 0, sizeof(VSConstants) };
		VkDescriptorBufferInfo psBuffer = { descriptors.psConstants, 0, sizeof(PSConstants) };
		VkDescriptorBufferInfo lightBuffer = { descriptors.lightConstants, 0, sizeof(LightConstants) };
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
		writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
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

	// Dynamic offsets in the order of the bindings
	if(setChanged || m_boundConstantOffsets[0] != uint32_t(m_vsConstants.offset)
	   || m_boundConstantOffsets[1] != uint32_t(m_psConstants.offset)
	   || m_boundConstantOffsets[2] != uint32_t(m_lights.offset)) {
		m_boundConstantOffsets[0] = uint32_t(m_vsConstants.offset);
		m_boundConstantOffsets[1] = uint32_t(m_psConstants.offset);
		m_boundConstantOffsets[2] = uint32_t(m_lights.offset);
		vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1,
		                        &m_boundDescriptorSet, uint32_t(std::size(m_boundConstantOffsets)),
		                        m_boundConstantOffsets);
	}

	return true;
}

void VulkanRenderer::setPixelLighting(bool enabled) {

	m_pixelLighting = enabled;

	if(!enabled) {
		setPixelLights(nullptr, 0);
	}

}

void VulkanRenderer::setPixelLights(const PixelLight * lights, size_t count) {

	if(m_lightData.size() != sizeof(LightConstants)) {
		return;
	}

	// Lights that only add highlights come last and are dropped first
	size_t diffuseCount = 0;
	while(diffuseCount < count && !lights[diffuseCount].specularOnly) {
		diffuseCount++;
	}
	if(diffuseCount > MaxPixelLights) {
		static bool warned = false;
		if(!warned) {
			LogWarning << "Too many lights for per-pixel lighting: " << diffuseCount << " > " << MaxPixelLights;
			warned = true;
		}
	}
	count = std::min(count, MaxPixelLights);
	diffuseCount = std::min(diffuseCount, count);

	LightConstants & constants = *reinterpret_cast<LightConstants *>(m_lightData.data());
	if(count == 0 && constants.count[2] == 0) {
		return;
	}
	constants.count[0] = s32(diffuseCount);
	constants.count[1] = 0; // No debug views
	constants.count[2] = s32(count);
	constants.count[3] = 0;
	for(size_t i = 0; i < count; i++) {
		const PixelLight & light = lights[i];
		float * a = constants.lights[i * 2];
		float * b = constants.lights[i * 2 + 1];
		a[0] = light.position.x;
		a[1] = light.position.y;
		a[2] = light.position.z;
		a[3] = light.fallStart;
		b[0] = light.color.r * light.intensity;
		b[1] = light.color.g * light.intensity;
		b[2] = light.color.b * light.intensity;
		b[3] = light.fallEnd;
	}
	m_lightDataSize = sizeof(LightConstants::count) + count * 2 * sizeof(LightConstants::lights[0]);

	// Copied again for the next draw, draws recorded before keep the old lights
	m_lights.frame = 0;

}

void VulkanRenderer::setNormalMaps(float strength) {

	bool wasEnabled = reliefMapsEnabled();
	m_normalMapStrength = glm::clamp(strength, 0.f, 1.f);
	updateReliefMaps(wasEnabled);

}

void VulkanRenderer::setParallaxMapping(float strength) {

	bool wasEnabled = reliefMapsEnabled();
	m_parallaxStrength = glm::clamp(strength, 0.f, 1.f);
	updateReliefMaps(wasEnabled);

}

void VulkanRenderer::setSpecular(float strength) {
	m_specularStrength = glm::clamp(strength, 0.f, 1.f);
}

void VulkanRenderer::updateReliefMaps(bool wasEnabled) {

	bool enabled = reliefMapsEnabled();
	if(enabled == wasEnabled) {
		return;
	}

	// Generate the relief maps for the loaded textures, or free them
	for(VulkanTexture & texture : m_textures) {
		if(enabled) {
			texture.updateReliefMap();
		} else {
			texture.releaseReliefMap();
		}
	}

}

VkPipeline VulkanRenderer::getReliefPipeline() {

	if(m_reliefPipeline || !m_reliefPixelShader) {
		return m_reliefPipeline;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }
	};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = m_blitVertexShader;
	stages[0].pName = "vsBlit";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = m_reliefPixelShader;
	stages[1].pName = "psNormalMap";

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
	rasterization.lineWidth = 1.f;
	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState blend = {};
	blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
	                       | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	colorBlend.attachmentCount = 1;
	colorBlend.pAttachments = &blend;
	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.dynamicStateCount = uint32_t(std::size(dynamicStates));
	dynamic.pDynamicStates = dynamicStates;

	const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	VkPipelineRenderingCreateInfoKHR rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachmentFormats = &format;

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
	info.layout = m_blitLayout.pipeline;

	if(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &info, nullptr, &m_reliefPipeline) != VK_SUCCESS) {
		LogError << "Failed to create the Vulkan relief map pipeline";
		m_reliefPipeline = VK_NULL_HANDLE;
		// Do not try again for every texture
		vkDestroyShaderModule(m_device, m_reliefPixelShader, nullptr);
		m_reliefPixelShader = VK_NULL_HANDLE;
	}

	return m_reliefPipeline;
}

void VulkanRenderer::createReliefMap(VkImageView color, const Vec2i & size, VkImage & image,
                                     VmaAllocation & allocation, VkImageView & view) {

	VkPipeline pipeline = getReliefPipeline();
	if(!pipeline || !color || size.x <= 0 || size.y <= 0) {
		return;
	}

	uint32_t width = uint32_t(size.x);
	uint32_t height = uint32_t(size.y);
	uint32_t levels = vulkanMipLevels(width, height);

	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.extent = { width, height, 1 };
	info.mipLevels = levels;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
	             | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VmaAllocationCreateInfo create = {};
	create.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	if(vmaCreateImage(m_allocator, &info, &create, &image, &allocation, nullptr) != VK_SUCCESS) {
		LogError << "Failed to create " << size.x << "x" << size.y << " Vulkan relief map";
		image = VK_NULL_HANDLE;
		allocation = VK_NULL_HANDLE;
		return;
	}

	// All levels for sampling, the first one for rendering
	VkImageView target = VK_NULL_HANDLE;
	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = info.format;
	viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
	bool success = (vkCreateImageView(m_device, &viewInfo, nullptr, &view) == VK_SUCCESS);
	viewInfo.subresourceRange.levelCount = 1;
	success = success && (vkCreateImageView(m_device, &viewInfo, nullptr, &target) == VK_SUCCESS);

	VkDescriptorSet set = success ? allocateUploadDescriptorSet(m_blitLayout.set) : VK_NULL_HANDLE;
	if(!set) {
		LogError << "Failed to prepare the Vulkan relief map";
		vkDestroyImageView(m_device, target, nullptr);
		vkDestroyImageView(m_device, view, nullptr);
		vmaDestroyImage(m_allocator, image, allocation);
		image = VK_NULL_HANDLE;
		allocation = VK_NULL_HANDLE;
		view = VK_NULL_HANDLE;
		return;
	}

	VkDescriptorBufferInfo buffer = { m_fullRectConstants, 0, sizeof(BlitConstants) };
	VkDescriptorImageInfo source = { VK_NULL_HANDLE, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo sampler = {
		getSampler(TextureStage::WrapClamp, TextureStage::FilterNearest, TextureStage::FilterNearest, false, 0.f),
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
	writes[1].pImageInfo = &source;
	writes[2].dstBinding = SamplerBinding;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	writes[2].pImageInfo = &sampler;
	vkUpdateDescriptorSets(m_device, uint32_t(std::size(writes)), writes, 0, nullptr);

	// After the upload of the texture and its mipmaps, see VulkanTexture::upload()
	VkCommandBuffer commands = beginUpload();

	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
	                     0, nullptr, 0, nullptr, 1, &barrier);

	VkRenderingAttachmentInfoKHR attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
	attachment.imageView = target;
	attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfoKHR rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
	rendering.renderArea = { { 0, 0 }, { width, height } };
	rendering.layerCount = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments = &attachment;
	m_cmdBeginRendering(commands, &rendering);

	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	uint32_t offset = 0;
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blitLayout.pipeline, 0, 1, &set, 1, &offset);
	VkViewport viewport = { 0.f, float(height), float(width), -float(height), 0.f, 1.f };
	vkCmdSetViewport(commands, 0, 1, &viewport);
	VkRect2D scissor = { { 0, 0 }, { width, height } };
	vkCmdSetScissor(commands, 0, 1, &scissor);
	vkCmdDraw(commands, 3, 1, 0, 0);

	m_cmdEndRendering(commands);

	recordVulkanMipmaps(commands, image, width, height, levels, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

	// The view for rendering is only used by the upload commands
	VkDevice device = m_device;
	m_upload.releases.push_back([device, target]() {
		vkDestroyImageView(device, target, nullptr);
	});

}
