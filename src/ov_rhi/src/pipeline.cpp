#define OV_LOG_CATEGORY "rhi"

#include "vulkan_common.hpp"

#include "ov/base/log.hpp"

#include <fstream>
#include <string>
#include <vector>

namespace ov::rhi {

namespace {

/// SPIR-V compiled offline by glslc, at build time.
///
/// There is deliberately no shader compiler in this process. shaderc costs tens
/// of megabytes of binary and a noticeable slice of start-up, to solve a
/// problem the build system already solved — and it makes a shader syntax error
/// a runtime failure on a user's machine instead of a red build.
[[nodiscard]] std::expected<std::vector<u32>, RhiError> read_spirv(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        OV_LOG_ERROR("shader not found: {}", path);
        return std::unexpected(RhiError::FileNotFound);
    }
    const auto size = static_cast<usize>(file.tellg());
    if (size == 0 || size % 4 != 0) {
        return std::unexpected(RhiError::BadShader);
    }
    file.seekg(0);

    std::vector<u32> words(size / 4);
    file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size));
    if (!file) {
        return std::unexpected(RhiError::FileNotFound);
    }
    return words;
}

[[nodiscard]] VkCompareOp to_vulkan(CompareOp op) {
    switch (op) {
        case CompareOp::Never: return VK_COMPARE_OP_NEVER;
        case CompareOp::Less: return VK_COMPARE_OP_LESS;
        case CompareOp::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
        case CompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_LESS;
}

[[nodiscard]] VkCullModeFlags to_vulkan(CullMode mode) {
    switch (mode) {
        case CullMode::None: return VK_CULL_MODE_NONE;
        case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
        case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    }
    return VK_CULL_MODE_NONE;
}

}  // namespace

std::expected<PipelineHandle, RhiError> Device::create_graphics_pipeline(
    const GraphicsPipelineDesc& desc) {
    Impl& impl = *impl_;

    const std::string directory =
        impl.desc.shader_directory.empty() ? std::string{} : impl.desc.shader_directory + "/";

    auto vertex_code = read_spirv(directory + std::string(desc.vertex_shader));
    if (!vertex_code) {
        return std::unexpected(vertex_code.error());
    }
    auto fragment_code = read_spirv(directory + std::string(desc.fragment_shader));
    if (!fragment_code) {
        return std::unexpected(fragment_code.error());
    }

    const auto make_module = [&impl](const std::vector<u32>& code) {
        VkShaderModuleCreateInfo info{};
        info.sType            = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize         = code.size() * sizeof(u32);
        info.pCode            = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        vkCreateShaderModule(impl.device, &info, nullptr, &module);
        return module;
    };

    VkShaderModule vertex_module   = make_module(*vertex_code);
    VkShaderModule fragment_module = make_module(*fragment_code);
    if (vertex_module == VK_NULL_HANDLE || fragment_module == VK_NULL_HANDLE) {
        vkDestroyShaderModule(impl.device, vertex_module, nullptr);
        vkDestroyShaderModule(impl.device, fragment_module, nullptr);
        return std::unexpected(RhiError::BadShader);
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName  = "main";

    std::vector<VkVertexInputBindingDescription>   bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (u32 index = 0; index < desc.vertex_bindings.size(); ++index) {
        const auto& binding = desc.vertex_bindings[index];
        bindings.push_back(VkVertexInputBindingDescription{
            index, binding.stride,
            binding.input_rate == VertexInputRate::Instance ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                            : VK_VERTEX_INPUT_RATE_VERTEX});
        for (const auto& attribute : binding.attributes) {
            attributes.push_back(VkVertexInputAttributeDescription{
                attribute.location, index, to_vulkan(attribute.format), attribute.offset});
        }
    }

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = static_cast<u32>(bindings.size());
    vertex_input.pVertexBindingDescriptions      = bindings.data();
    vertex_input.vertexAttributeDescriptionCount = static_cast<u32>(attributes.size());
    vertex_input.pVertexAttributeDescriptions    = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = desc.topology == PrimitiveTopology::LineList
                            ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                            : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport and scissor are dynamic. A pipeline baked against a window size
    // has to be rebuilt on every resize, which on a driver that compiles
    // shaders lazily is a visible stall.
    const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                       VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo    dynamic{};
    dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<u32>(dynamic_states.size());
    dynamic.pDynamicStates    = dynamic_states.data();

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = to_vulkan(desc.cull_mode);
    // Counter-clockwise: the baker winds every quad so that its normal points
    // out of the block, and that is the winding this has to agree with.
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable  = desc.depth_test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp   = to_vulkan(desc.depth_compare);
    depth.maxDepthBounds   = 1.0F;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (desc.blend == BlendMode::Alpha) {
        blend.blendEnable         = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp        = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp        = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments    = &blend;

    // ── Layout ──────────────────────────────────────────────────────────────
    PipelineResource resource;
    resource.sampled_image_count  = desc.layout.sampled_image_count;
    resource.storage_buffer_count = desc.layout.storage_buffer_count;
    resource.push_constant_size   = desc.layout.push_constant_size;

    std::vector<VkDescriptorSetLayoutBinding> set_bindings;
    for (u32 i = 0; i < desc.layout.sampled_image_count; ++i) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = i;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        set_bindings.push_back(binding);
    }
    // Vertex stage: the one thing these carry today is the per-section origin,
    // read once per vertex now that the draw cannot push it.
    for (u32 i = 0; i < desc.layout.storage_buffer_count; ++i) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = desc.layout.sampled_image_count + i;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        set_bindings.push_back(binding);
    }

    if (!set_bindings.empty()) {
        VkDescriptorSetLayoutCreateInfo set_info{};
        set_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_info.bindingCount = static_cast<u32>(set_bindings.size());
        set_info.pBindings    = set_bindings.data();
        vkCreateDescriptorSetLayout(impl.device, &set_info, nullptr, &resource.set_layout);
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size       = desc.layout.push_constant_size;

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount         = resource.set_layout != VK_NULL_HANDLE ? 1u : 0u;
    layout_info.pSetLayouts            = &resource.set_layout;
    layout_info.pushConstantRangeCount = desc.layout.push_constant_size > 0 ? 1u : 0u;
    layout_info.pPushConstantRanges    = &push;
    vkCreatePipelineLayout(impl.device, &layout_info, nullptr, &resource.layout);

    // ── Dynamic rendering: no VkRenderPass, no VkFramebuffer ────────────────
    const VkFormat colour_format = to_vulkan(desc.colour_format);

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount    = 1;
    rendering.pColorAttachmentFormats = &colour_format;
    rendering.depthAttachmentFormat =
        desc.depth_test || desc.depth_write ? to_vulkan(desc.depth_format) : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &rendering;
    info.stageCount          = static_cast<u32>(stages.size());
    info.pStages             = stages.data();
    info.pVertexInputState   = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState      = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &multisample;
    info.pDepthStencilState  = &depth;
    info.pColorBlendState    = &blending;
    info.pDynamicState       = &dynamic;
    info.layout              = resource.layout;

    const VkResult created = vkCreateGraphicsPipelines(impl.device, impl.pipeline_cache, 1, &info,
                                                       nullptr, &resource.pipeline);

    vkDestroyShaderModule(impl.device, vertex_module, nullptr);
    vkDestroyShaderModule(impl.device, fragment_module, nullptr);

    if (created != VK_SUCCESS) {
        vkDestroyPipelineLayout(impl.device, resource.layout, nullptr);
        if (resource.set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(impl.device, resource.set_layout, nullptr);
        }
        return std::unexpected(RhiError::BadShader);
    }

    // Written to disk at teardown, read at start-up. On MoltenVK the driver
    // translates SPIR-V to Metal, and that is not free the first time.
    impl.save_pipeline_cache();

    return impl.pipelines.insert(resource);
}

}  // namespace ov::rhi
