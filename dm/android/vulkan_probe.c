/* Vulkan device check using translated original game shaders, never replacements.
 * Input order: default-CS, counter-clear-CS, embedded-color-VS, embedded-color-PS,
 * output RGBA path. The optional draw uses harness geometry and a harness matrix;
 * it is not a game frame or proof that the game renderer works.
 */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 64, HEIGHT = 64, SUPPORT_BYTES = 1392, UNIFORM_BYTES = 65536 };
typedef struct { VkBuffer buffer; VkDeviceMemory memory; void *mapped; VkDeviceSize bytes; } Buffer;
typedef struct {
    VkInstance instance; VkPhysicalDevice physical; VkDevice device; VkQueue queue;
    uint32_t family; VkCommandPool commands; VkPhysicalDeviceMemoryProperties memory;
} Context;

static int fail(const char *operation, VkResult result) {
    fprintf(stderr, "{\"operation\":\"%s\",\"vk_result\":%d}\n", operation, (int)result);
    return 0;
}
#define VK_OK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) return fail(#call, r_); } while (0)

static int memory_type(Context *c, uint32_t bits, VkMemoryPropertyFlags flags, uint32_t *index) {
    for (uint32_t i = 0; i < c->memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (c->memory.memoryTypes[i].propertyFlags & flags) == flags) {
            *index = i; return 1;
        }
    }
    return fail("memory_type", VK_ERROR_FEATURE_NOT_PRESENT);
}
static int make_buffer(Context *c, Buffer *b, VkDeviceSize bytes, VkBufferUsageFlags usage) {
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_OK(vkCreateBuffer(c->device, &info, NULL, &b->buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(c->device, b->buffer, &requirements);
    uint32_t type = 0;
    if (!memory_type(c, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) return 0;
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = type };
    VK_OK(vkAllocateMemory(c->device, &alloc, NULL, &b->memory));
    VK_OK(vkBindBufferMemory(c->device, b->buffer, b->memory, 0));
    VK_OK(vkMapMemory(c->device, b->memory, 0, bytes, 0, &b->mapped));
    b->bytes = bytes; memset(b->mapped, 0, (size_t)bytes); return 1;
}
static void free_buffer(Context *c, Buffer *b) {
    if (b->mapped) vkUnmapMemory(c->device, b->memory);
    if (b->buffer) vkDestroyBuffer(c->device, b->buffer, NULL);
    if (b->memory) vkFreeMemory(c->device, b->memory, NULL);
    memset(b, 0, sizeof(*b));
}
static void fill_support(Buffer *b) {
    for (unsigned i = 0; i < 73; ++i) {
        const float value = 1.0f;
        memcpy((uint8_t *)b->mapped + 192 + 16 * i, &value, sizeof(value));
    }
}
static int shader(Context *c, const char *path, VkShaderModule *module) {
    FILE *file = fopen(path, "rb");
    if (!file) return fail("open_original_shader", VK_ERROR_INITIALIZATION_FAILED);
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    long size = ftell(file);
    if (size < 20 || size > 16 * 1024 * 1024 || size % 4 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); return fail("original_shader_size", VK_ERROR_INITIALIZATION_FAILED);
    }
    uint32_t *code = malloc((size_t)size);
    if (!code) { fclose(file); return 0; }
    size_t got = fread(code, 1, (size_t)size, file); fclose(file);
    if (got != (size_t)size || code[0] != 0x07230203) { free(code); return 0; }
    VkShaderModuleCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)size, .pCode = code };
    VkResult result = vkCreateShaderModule(c->device, &info, NULL, module);
    free(code);
    return result == VK_SUCCESS ? 1 : fail("vkCreateShaderModule", result);
}
static int begin(Context *c, VkCommandBuffer *command) {
    VkCommandBufferAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = c->commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VK_OK(vkAllocateCommandBuffers(c->device, &alloc, command));
    VkCommandBufferBeginInfo info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_OK(vkBeginCommandBuffer(*command, &info)); return 1;
}
static int submit(Context *c, VkCommandBuffer command) {
    VK_OK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE; VK_OK(vkCreateFence(c->device, &fi, NULL, &fence));
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command };
    VkResult result = vkQueueSubmit(c->queue, 1, &submit, fence);
    if (result == VK_SUCCESS) result = vkWaitForFences(c->device, 1, &fence, VK_TRUE, 10000000000ULL);
    vkDestroyFence(c->device, fence, NULL);
    vkFreeCommandBuffers(c->device, c->commands, 1, &command);
    return result == VK_SUCCESS ? 1 : fail("submit_and_fence", result);
}
static int compute(Context *c, const char *path, int writes_counter, uint32_t *first, uint32_t *second) {
    Buffer support = {0}, storage = {0};
    if (!make_buffer(c, &support, SUPPORT_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !make_buffer(c, &storage, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
    fill_support(&support);
    ((uint32_t *)storage.mapped)[0] = 0xdeadbeef;
    ((uint32_t *)storage.mapped)[1] = 0xcafebabe;
    VkShaderModule module = VK_NULL_HANDLE;
    if (!shader(c, path, &module)) return 0;
    VkDescriptorSetLayoutBinding bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = writes_counter ? 2u : 1u, .pBindings = bindings };
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VK_OK(vkCreateDescriptorSetLayout(c->device, &li, NULL, &set_layout));
    VkPipelineLayoutCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VK_OK(vkCreatePipelineLayout(c->device, &pi, NULL, &layout));
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" },
        .layout = layout };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_OK(vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = writes_counter ? 2u : 1u, .pPoolSizes = sizes };
    VkDescriptorPool pool = VK_NULL_HANDLE; VK_OK(vkCreateDescriptorPool(c->device, &dpi, NULL, &pool));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout };
    VkDescriptorSet set = VK_NULL_HANDLE; VK_OK(vkAllocateDescriptorSets(c->device, &dai, &set));
    VkDescriptorBufferInfo bi[2] = {{support.buffer, 0, SUPPORT_BYTES}, {storage.buffer, 0, 64}};
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1] }
    };
    vkUpdateDescriptorSets(c->device, writes_counter ? 2 : 1, writes, 0, NULL);
    VkCommandBuffer command = VK_NULL_HANDLE; if (!begin(c, &command)) return 0;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(command, 1, 1, 1);
    VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &barrier, 0, NULL, 0, NULL);
    if (!submit(c, command)) return 0;
    *first = ((uint32_t *)storage.mapped)[0]; *second = ((uint32_t *)storage.mapped)[1];
    int correct = *first == (writes_counter ? 0u : 0xdeadbeefu) && *second == 0xcafebabeu;
    vkDestroyDescriptorPool(c->device, pool, NULL); vkDestroyPipeline(c->device, pipeline, NULL);
    vkDestroyPipelineLayout(c->device, layout, NULL); vkDestroyDescriptorSetLayout(c->device, set_layout, NULL);
    vkDestroyShaderModule(c->device, module, NULL); free_buffer(c, &storage); free_buffer(c, &support);
    return correct;
}

static int graphics(Context *c, const char *vs_path, const char *ps_path, const char *output,
                    uint32_t *colored, uint32_t *clear) {
    Buffer support = {0}, uniforms = {0}, vertices = {0}, readback = {0};
    if (!make_buffer(c, &support, SUPPORT_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !make_buffer(c, &uniforms, UNIFORM_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !make_buffer(c, &vertices, 3 * 8 * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !make_buffer(c, &readback, WIDTH * HEIGHT * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return 0;
    fill_support(&support);
    float *matrix = uniforms.mapped;
    for (unsigned i = 0; i < 4; ++i) matrix[i * 5] = 1.0f;
    const float fixture[24] = {
        -.75f,-.75f,.5f,1.f, 1.f,.25f,.125f,1.f,
         .75f,-.75f,.5f,1.f, 1.f,.25f,.125f,1.f,
         0.f, .75f,.5f,1.f, 1.f,.25f,.125f,1.f
    };
    memcpy(vertices.mapped, fixture, sizeof(fixture));
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
    if (!shader(c, vs_path, &vs) || !shader(c, ps_path, &ps)) return 0;
    VkDescriptorSetLayoutBinding vb[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT }
    };
    VkDescriptorSetLayoutBinding pb = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = vb };
    VkDescriptorSetLayout sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VK_OK(vkCreateDescriptorSetLayout(c->device, &li, NULL, &sets[0]));
    li.bindingCount = 1; li.pBindings = &pb;
    VK_OK(vkCreateDescriptorSetLayout(c->device, &li, NULL, &sets[1]));
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2, .pSetLayouts = sets };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VK_OK(vkCreatePipelineLayout(c->device, &pli, NULL, &layout));
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3};
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &size };
    VkDescriptorPool pool = VK_NULL_HANDLE; VK_OK(vkCreateDescriptorPool(c->device, &dpi, NULL, &pool));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 2, .pSetLayouts = sets };
    VkDescriptorSet descriptors[2]; VK_OK(vkAllocateDescriptorSets(c->device, &dai, descriptors));
    VkDescriptorBufferInfo bis[3] = {{support.buffer,0,SUPPORT_BYTES},{uniforms.buffer,0,UNIFORM_BYTES},
                                    {support.buffer,0,SUPPORT_BYTES}};
    VkWriteDescriptorSet writes[3]; memset(writes, 0, sizeof(writes));
    for (unsigned i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptors[i == 2 ? 1 : 0]; writes[i].dstBinding = i == 1 ? 1 : 0;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &bis[i];
    }
    vkUpdateDescriptorSets(c->device, 3, writes, 0, NULL);
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {WIDTH, HEIGHT, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage image = VK_NULL_HANDLE; VK_OK(vkCreateImage(c->device, &ii, NULL, &image));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(c->device, image, &mr);
    uint32_t type = 0; if (!memory_type(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) return 0;
    VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = type };
    VkDeviceMemory image_memory = VK_NULL_HANDLE; VK_OK(vkAllocateMemory(c->device, &mi, NULL, &image_memory));
    VK_OK(vkBindImageMemory(c->device, image, image_memory, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1} };
    VkImageView view = VK_NULL_HANDLE; VK_OK(vkCreateImageView(c->device, &vi, NULL, &view));
    VkAttachmentDescription attachment = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color };
    VkSubpassDependency dependency = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    VkRenderPassCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass,
        .dependencyCount = 1, .pDependencies = &dependency };
    VkRenderPass pass = VK_NULL_HANDLE; VK_OK(vkCreateRenderPass(c->device, &ri, NULL, &pass));
    VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
        .attachmentCount = 1, .pAttachments = &view, .width = WIDTH, .height = HEIGHT, .layers = 1 };
    VkFramebuffer framebuffer = VK_NULL_HANDLE; VK_OK(vkCreateFramebuffer(c->device, &fi, NULL, &framebuffer));
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = ps, .pName = "main" }
    };
    VkVertexInputBindingDescription binding = {0, 8 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[2] = {
        {0,0,VK_FORMAT_R32G32B32A32_SFLOAT,0}, {1,0,VK_FORMAT_R32G32B32A32_SFLOAT,4*sizeof(float)}
    };
    VkPipelineVertexInputStateCreateInfo vertex = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attributes };
    VkPipelineInputAssemblyStateCreateInfo assembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport viewport = {0,0,WIDTH,HEIGHT,0,1}; VkRect2D scissor = {{0,0},{WIDTH,HEIGHT}};
    VkPipelineViewportStateCreateInfo viewport_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor };
    VkPipelineRasterizationStateCreateInfo raster = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo multisample = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState blend = { .colorWriteMask = 15 };
    VkPipelineColorBlendStateCreateInfo blend_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend };
    VkGraphicsPipelineCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport_info, .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend_info, .layout = layout, .renderPass = pass, .subpass = 0 };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_OK(vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gi, NULL, &pipeline));
    VkCommandBuffer command = VK_NULL_HANDLE; if (!begin(c, &command)) return 0;
    VkClearValue clear_value = { .color = {{0,0,0,1}} };
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
        .framebuffer = framebuffer, .renderArea = {{0,0},{WIDTH,HEIGHT}}, .clearValueCount = 1,
        .pClearValues = &clear_value };
    vkCmdBeginRenderPass(command, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, descriptors, 0, NULL);
    VkDeviceSize offset = 0; vkCmdBindVertexBuffers(command, 0, 1, &vertices.buffer, &offset);
    vkCmdDraw(command, 3, 1, 0, 0); vkCmdEndRenderPass(command);
    VkBufferImageCopy copy = { .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent = {WIDTH,HEIGHT,1} };
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (!submit(c, command)) return 0;
    *colored = 0; *clear = 0;
    const uint8_t *pixels = readback.mapped;
    for (unsigned i = 0; i < WIDTH * HEIGHT; ++i) {
        const uint8_t *p = pixels + i * 4;
        if (p[0] >= 250 && p[1] >= 60 && p[1] <= 68 && p[2] >= 28 && p[2] <= 36 && p[3] == 255) ++*colored;
        else if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255) ++*clear;
    }
    FILE *file = fopen(output, "wb");
    if (!file || fwrite(pixels, 1, WIDTH * HEIGHT * 4, file) != WIDTH * HEIGHT * 4) {
        if (file) fclose(file); return fail("write_rgba_readback", VK_ERROR_INITIALIZATION_FAILED);
    }
    fclose(file);
    int correct = *colored > 0 && *clear > 0 && *colored + *clear == WIDTH * HEIGHT;
    vkDestroyPipeline(c->device,pipeline,NULL); vkDestroyFramebuffer(c->device,framebuffer,NULL);
    vkDestroyRenderPass(c->device,pass,NULL); vkDestroyImageView(c->device,view,NULL);
    vkDestroyImage(c->device,image,NULL); vkFreeMemory(c->device,image_memory,NULL);
    vkDestroyDescriptorPool(c->device,pool,NULL); vkDestroyPipelineLayout(c->device,layout,NULL);
    for (unsigned i=0;i<2;++i) vkDestroyDescriptorSetLayout(c->device,sets[i],NULL);
    vkDestroyShaderModule(c->device,vs,NULL); vkDestroyShaderModule(c->device,ps,NULL);
    free_buffer(c,&readback); free_buffer(c,&vertices); free_buffer(c,&uniforms); free_buffer(c,&support);
    return correct;
}

static int initialize(Context *c) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "d2r-original-shader-probe", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VK_OK(vkCreateInstance(&info,NULL,&c->instance));
    uint32_t count = 0; VK_OK(vkEnumeratePhysicalDevices(c->instance,&count,NULL));
    if (!count) return fail("no_physical_device",VK_ERROR_INITIALIZATION_FAILED);
    VkPhysicalDevice *devices = calloc(count,sizeof(*devices)); if (!devices) return 0;
    VkResult result = vkEnumeratePhysicalDevices(c->instance,&count,devices);
    if (result != VK_SUCCESS) { free(devices); return fail("physical_devices",result); }
    c->physical = devices[0]; free(devices);
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(c->physical,&properties);
    VkPhysicalDeviceFeatures features; vkGetPhysicalDeviceFeatures(c->physical,&features);
    VkPhysicalDeviceSubgroupProperties subgroup = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    VkPhysicalDeviceProperties2 properties2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &subgroup };
    PFN_vkGetPhysicalDeviceProperties2 get_properties2 =
        (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(c->instance,"vkGetPhysicalDeviceProperties2");
    if (!get_properties2) return fail("vkGetPhysicalDeviceProperties2_missing",VK_ERROR_FEATURE_NOT_PRESENT);
    get_properties2(c->physical,&properties2);
    printf("{\"schema\":1,\"gpu\":\"%s\",\"api_version_raw\":%u,\"features\":{"
           "\"shader_float64\":%s,\"shader_int64\":%s,\"storage_image_read_without_format\":%s,"
           "\"storage_image_write_without_format\":%s,\"geometry_shader\":%s,\"tessellation_shader\":%s},"
           "\"subgroup\":{\"size\":%u,\"stages\":%u,\"operations\":%u},",
           properties.deviceName,properties.apiVersion,features.shaderFloat64?"true":"false",
           features.shaderInt64?"true":"false",features.shaderStorageImageReadWithoutFormat?"true":"false",
           features.shaderStorageImageWriteWithoutFormat?"true":"false",features.geometryShader?"true":"false",
           features.tessellationShader?"true":"false",subgroup.subgroupSize,subgroup.supportedStages,subgroup.supportedOperations);
    fflush(stdout);
    vkGetPhysicalDeviceMemoryProperties(c->physical,&c->memory);
    uint32_t queues = 0; vkGetPhysicalDeviceQueueFamilyProperties(c->physical,&queues,NULL);
    VkQueueFamilyProperties *families = calloc(queues,sizeof(*families)); if (!families) return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->physical,&queues,families);
    c->family = UINT32_MAX;
    for (uint32_t i=0;i<queues;++i) if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) ==
        (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) { c->family=i; break; }
    free(families); if (c->family==UINT32_MAX) return 0;
    float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=c->family,
        .queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceFeatures enabled={0}; /* The selected four shaders require Shader only. */
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,
        .pQueueCreateInfos=&qi,.pEnabledFeatures=&enabled};
    VK_OK(vkCreateDevice(c->physical,&di,NULL,&c->device));
    vkGetDeviceQueue(c->device,c->family,0,&c->queue);
    VkCommandPoolCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=c->family};
    VK_OK(vkCreateCommandPool(c->device,&ci,NULL,&c->commands)); return 1;
}

int main(int argc,char **argv) {
    if (argc != 6) { fprintf(stderr,"usage: vulkan_probe defaultCS clearCS colorVS colorPS output.rgba\n"); return 2; }
    Context c={0}; if (!initialize(&c)) return 3;
    uint32_t default_first=0,default_second=0,clear_first=0,clear_second=0,colored=0,black=0;
    int a=compute(&c,argv[1],0,&default_first,&default_second);
    int b=a?compute(&c,argv[2],1,&clear_first,&clear_second):0;
    int g=b?graphics(&c,argv[3],argv[4],argv[5],&colored,&black):0;
    printf("\"default_compute\":{\"passed\":%s,\"original_kernel_is_empty\":true,\"fence_waited\":%s,"
           "\"word0\":%u,\"word1\":%u},\"counter_clear_compute\":{\"passed\":%s,\"word0\":%u,\"word1\":%u},"
           "\"graphics\":{\"passed\":%s,\"width\":%d,\"height\":%d,\"colored_pixels\":%u,\"clear_pixels\":%u,"
           "\"geometry_and_matrix\":\"harness triangle and identity projection\",\"original_shader_pair\":true},"
           "\"game_first_frame_verified\":false,\"game_scene_verified\":false,\"passed\":%s}\n",
           a?"true":"false",a?"true":"false",default_first,default_second,b?"true":"false",clear_first,clear_second,
           g?"true":"false",WIDTH,HEIGHT,colored,black,(a&&b&&g)?"true":"false");
    vkDeviceWaitIdle(c.device); vkDestroyCommandPool(c.device,c.commands,NULL);
    vkDestroyDevice(c.device,NULL); vkDestroyInstance(c.instance,NULL); return a&&b&&g?0:4;
}
