#include <lug/Graphics/Vulkan/Render/SkyBox.hpp>

#include <lug/Graphics/Builder/Texture.hpp>
#include <lug/Graphics/Vulkan/API/Builder/CommandBuffer.hpp>
#include <lug/Graphics/Vulkan/API/Builder/CommandPool.hpp>
#include <lug/Graphics/Vulkan/API/Builder/DescriptorPool.hpp>
#include <lug/Graphics/Vulkan/API/Builder/DescriptorSet.hpp>
#include <lug/Graphics/Vulkan/API/Builder/Fence.hpp>
#include <lug/Graphics/Vulkan/API/Builder/Framebuffer.hpp>
#include <lug/Graphics/Vulkan/API/Builder/Image.hpp>
#include <lug/Graphics/Vulkan/API/Builder/ImageView.hpp>
#include <lug/Graphics/Vulkan/Render/Texture.hpp>
#include <lug/Graphics/Vulkan/Renderer.hpp>
#include <lug/Math/Constant.hpp>
#include <lug/Math/Geometry/Transform.hpp>
#include <lug/Math/Geometry/Trigonometry.hpp>

namespace lug {
namespace Graphics {
namespace Vulkan {
namespace Render {

lug::Graphics::Resource::SharedPtr<lug::Graphics::Render::Mesh> SkyBox::_mesh;
lug::Graphics::Resource::SharedPtr<lug::Graphics::Render::Texture> SkyBox::_brdfLut;
uint32_t SkyBox::_skyBoxCount{0};

SkyBox::SkyBox(const std::string& name) : ::lug::Graphics::Render::SkyBox(name) {}

SkyBox::~SkyBox() {
    destroy();
}

void SkyBox::destroy() {
    --_skyBoxCount;
}

Resource::SharedPtr<lug::Graphics::Render::SkyBox> SkyBox::createIrradianceMap(lug::Graphics::Renderer& renderer) const {
    constexpr uint32_t irradianceMapSize = 64;

    // Constructor of SkyBox is private, we can't use std::make_unique
    std::unique_ptr<Resource> resource{new Vulkan::Render::SkyBox(_name + "_irradiance_map")};
    Vulkan::Render::SkyBox* irradianceMap = static_cast<Vulkan::Render::SkyBox*>(resource.get());

    Vulkan::Renderer& vkRenderer = static_cast<Vulkan::Renderer&>(renderer);

    auto irradianceMapPipeline = vkRenderer.getPipeline(Render::Pipeline::getIrradianceMapBaseId());

    if (!irradianceMapPipeline) {
        LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::createIrradianceMap: Can't get the irradiance map pipeline");
        return nullptr;
    }

    const Resource::SharedPtr<lug::Graphics::Vulkan::Render::Texture> texture = Resource::SharedPtr<Render::Texture>::cast(getEnvironnementTexture());
    if (!texture) {
        LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::createIrradianceMap: The skybox doesn't have an environnement");
        return nullptr;
    }

    lug::Graphics::Builder::Texture textureBuilder(vkRenderer);

    textureBuilder.setType(lug::Graphics::Builder::Texture::Type::CubeMap);
    textureBuilder.setMagFilter(texture->getMagFilter());
    textureBuilder.setMinFilter(texture->getMinFilter());
    textureBuilder.setMipMapFilter(texture->getMipMapFilter());
    textureBuilder.setWrapS(texture->getWrapS());
    textureBuilder.setWrapT(texture->getWrapT());

    // TODO: Check which format to use
    if (!textureBuilder.addLayer(irradianceMapSize, irradianceMapSize, lug::Graphics::Render::Texture::Format::R32G32B32A32_SFLOAT)
        || !textureBuilder.addLayer(irradianceMapSize, irradianceMapSize, lug::Graphics::Render::Texture::Format::R32G32B32A32_SFLOAT)
        || !textureBuilder.addLayer(irradianceMapSize, irradianceMapSize, lug::Graphics::Render::Texture::Format::R32G32B32A32_SFLOAT)
        || !textureBuilder.addLayer(irradianceMapSize, irradianceMapSize, lug::Graphics::Render::Texture::Format::R32G32B32A32_SFLOAT)
        || !textureBuilder.addLayer(irradianceMapSize, irradianceMapSize, lug::Graphics::Render::Texture::Format::R32G32B32A32_SFLOAT)
        || !textureBuilder.addLayer(irradianceMapSize, irradianceMapSize, lug::Graphics::Render::Texture::Format::R32G32B32A32_SFLOAT)) {
        LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::build: Can't create irradiance map texture layers");
        return nullptr;
    }

    irradianceMap->_environnementTexture = textureBuilder.build();
    if (!irradianceMap->_environnementTexture) {
        LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::build Can't create the irradiance map texture");
        return nullptr;
    }

    // Create Framebuffer for irradiance map generation
    API::CommandPool commandPool;
    API::CommandBuffer cmdBuffer;
    API::Image offscreenImage;
    API::ImageView offscreenImageView;
    API::DeviceMemory imagesMemory;
    API::Framebuffer framebuffer;
    API::Fence fence;
    API::DescriptorPool descriptorPool;
    API::DescriptorSet descriptorSet;
    const API::Queue* graphicsQueue = nullptr;
    VkResult result{VK_SUCCESS};
    {
        // Graphics queue
        graphicsQueue = vkRenderer.getDevice().getQueue("queue_graphics");
        if (!graphicsQueue) {
            LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::createIrradianceMap: Can't find queue with name queue_graphics");
            return nullptr;
        }

        // Command pool
        API::Builder::CommandPool commandPoolBuilder(vkRenderer.getDevice(), *graphicsQueue->getQueueFamily());
        if (!commandPoolBuilder.build(commandPool, &result)) {
            LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createIrradianceMap: Can't create the graphics command pool: {}", result);
            return nullptr;
        }

        // Command buffer
        API::Builder::CommandBuffer commandBufferBuilder(vkRenderer.getDevice(), commandPool);
        commandBufferBuilder.setLevel(VK_COMMAND_BUFFER_LEVEL_PRIMARY);

        // Create the render command buffer
        if (!commandBufferBuilder.build(cmdBuffer, &result)) {
            LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::createIrradianceMap: Can't create the command buffer: {}", result);
            return nullptr;
        }

        // Descriptor pool
        {
            API::Builder::DescriptorPool descriptorPoolBuilder(vkRenderer.getDevice());

            descriptorPoolBuilder.setFlags(0);

            descriptorPoolBuilder.setMaxSets(42);

            std::vector<VkDescriptorPoolSize> poolSizes{
                {
                    /* poolSize.type            */ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    /* poolSize.descriptorCount */ 42
                }
            };
            descriptorPoolBuilder.setPoolSizes(poolSizes);

            if (!descriptorPoolBuilder.build(descriptorPool, &result)) {
                LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::createIrradianceMap: Can't create the descriptor pool: {}", result);
                return nullptr;
            }
        }

        // Descriptor set
        {
            API::Builder::DescriptorSet descriptorSetBuilder(vkRenderer.getDevice(), descriptorPool);
            descriptorSetBuilder.setDescriptorSetLayouts({static_cast<VkDescriptorSetLayout>(irradianceMapPipeline->getPipelineAPI().getLayout()->getDescriptorSetLayouts()[0])});

            if (!descriptorSetBuilder.build(descriptorSet, &result)) {
                LUG_LOG.error("DescriptorSetPool: Can't create descriptor set: {}", result);
                return nullptr;
            }

            descriptorSet.updateImages(
                0,
                0,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                {
                    {
                        /* sampler       */ static_cast<VkSampler>(texture->getSampler()),
                        /* imageView     */ static_cast<VkImageView>(texture->getImageView()),
                        /* imageLayout   */ VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    }
                }
            );
        }

        // Fence
        {
            API::Builder::Fence fenceBuilder(vkRenderer.getDevice());

            if (!fenceBuilder.build(fence, &result)) {
                LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::createIrradianceMap: Can't create render fence: {}", result);
                return nullptr;
            }
        }

        // Depth and offscreen images
        {
            API::Builder::DeviceMemory deviceMemoryBuilder(vkRenderer.getDevice());
            deviceMemoryBuilder.setMemoryFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            // Create image and add it to API::Builder::DeviceMemory
            const VkExtent3D extent{
                /* extent.width     */ irradianceMapSize,
                /* extent.height    */ irradianceMapSize,
                /* extent.depth     */ 1
            };

            // Create offscreen image
            {
                API::Builder::Image imageBuilder(vkRenderer.getDevice());

                imageBuilder.setExtent(extent);
                imageBuilder.setUsage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                imageBuilder.setPreferedFormats({ Resource::SharedPtr<Render::Texture>::cast(irradianceMap->_environnementTexture)->getImage().getFormat() });
                imageBuilder.setQueueFamilyIndices({ graphicsQueue->getQueueFamily()->getIdx() });
                imageBuilder.setTiling(VK_IMAGE_TILING_OPTIMAL);

                if (!imageBuilder.build(offscreenImage, &result)) {
                    LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createIrradianceMap: Can't create depth buffer image: {}", result);
                    return nullptr;
                }

                if (!deviceMemoryBuilder.addImage(offscreenImage)) {
                    LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createIrradianceMap: Can't add image to device memory");
                    return nullptr;
                }
            }

            // Initialize depth buffer memory
            {
                if (!deviceMemoryBuilder.build(imagesMemory, &result)) {
                    LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createIrradianceMap: Can't create device memory: {}", result);
                    return nullptr;
                }
            }

            // Create offscreen image view
            {
                API::Builder::ImageView imageViewBuilder(vkRenderer.getDevice(), offscreenImage);

                imageViewBuilder.setFormat(offscreenImage.getFormat());
                imageViewBuilder.setAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);

                if (!imageViewBuilder.build(offscreenImageView, &result)) {
                    LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createIrradianceMap: Can't create depth buffer image view: {}", result);
                    return nullptr;
                }
            }
        }

        // Create framebuffer
        {
            // Create depth buffer image view
            API::Builder::Framebuffer framebufferBuilder(vkRenderer.getDevice());

            const API::RenderPass* renderPass = irradianceMapPipeline->getPipelineAPI().getRenderPass();

            framebufferBuilder.setRenderPass(renderPass);
            framebufferBuilder.addAttachment(&offscreenImageView);
            framebufferBuilder.setWidth(irradianceMapSize);
            framebufferBuilder.setHeight(irradianceMapSize);

            if (!framebufferBuilder.build(framebuffer, &result)) {
                LUG_LOG.error("Forward::initFramebuffers: Can't create framebuffer: {}", result);
                return nullptr;
            }
        }
    }

    // Generate the irradiance cube map
    {
        if (!cmdBuffer.begin()) {
            return nullptr;
        }

        cmdBuffer.bindPipeline(irradianceMapPipeline->getPipelineAPI());

        // Bind descriptor set of the skybox
        {
            const API::CommandBuffer::CmdBindDescriptors skyBoxBind{
                /* skyBoxBind.pipelineLayout     */ *irradianceMapPipeline->getPipelineAPI().getLayout(),
                /* skyBoxBind.pipelineBindPoint  */ VK_PIPELINE_BIND_POINT_GRAPHICS,
                /* skyBoxBind.firstSet           */ 0,
                /* skyBoxBind.descriptorSets     */ {&descriptorSet},
                /* skyBoxBind.dynamicOffsets     */ {},
            };

            cmdBuffer.bindDescriptorSets(skyBoxBind);
        }

        auto& primitiveSet = SkyBox::getMesh()->getPrimitiveSets()[0];

        cmdBuffer.bindVertexBuffers(
            {static_cast<API::Buffer*>(primitiveSet.position->_data)},
            {0}
        );

        API::Buffer* indicesBuffer = static_cast<API::Buffer*>(primitiveSet.indices->_data);
        cmdBuffer.bindIndexBuffer(*indicesBuffer, VK_INDEX_TYPE_UINT16);
        const API::CommandBuffer::CmdDrawIndexed cmdDrawIndexed {
            /* cmdDrawIndexed.indexCount    */ primitiveSet.indices->buffer.elementsCount,
            /* cmdDrawIndexed.instanceCount */ 1,
        };

        Math::Mat4x4f projection = Math::Geometry::perspective(Math::halfPi<float>(), 1.0f, 0.1f, static_cast<float>(irradianceMapSize));
        std::vector<Math::Mat4x4f> matrices = {
            Math::Geometry::lookAt<float>({0.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}),
            Math::Geometry::lookAt<float>({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}),
            Math::Geometry::lookAt<float>({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}),
            Math::Geometry::lookAt<float>({0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}),
            Math::Geometry::lookAt<float>({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}),
            Math::Geometry::lookAt<float>({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f})
        };

        // Change offscreen image layout to VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL for render
        {
            API::CommandBuffer::CmdPipelineBarrier pipelineBarrier;
            pipelineBarrier.imageMemoryBarriers.resize(1);
            pipelineBarrier.imageMemoryBarriers[0].srcAccessMask = 0;
            pipelineBarrier.imageMemoryBarriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            pipelineBarrier.imageMemoryBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            pipelineBarrier.imageMemoryBarriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            pipelineBarrier.imageMemoryBarriers[0].image = &offscreenImage;

            cmdBuffer.pipelineBarrier(pipelineBarrier);
        }

        // Change irradiance layout to VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL for copy
        {
            API::CommandBuffer::CmdPipelineBarrier pipelineBarrier;
            pipelineBarrier.imageMemoryBarriers.resize(1);
            pipelineBarrier.imageMemoryBarriers[0].srcAccessMask = 0;
            pipelineBarrier.imageMemoryBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            pipelineBarrier.imageMemoryBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            pipelineBarrier.imageMemoryBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            pipelineBarrier.imageMemoryBarriers[0].image = &Resource::SharedPtr<Render::Texture>::cast(irradianceMap->_environnementTexture)->getImage();
            pipelineBarrier.imageMemoryBarriers[0].subresourceRange.layerCount = 6;

            cmdBuffer.pipelineBarrier(pipelineBarrier);
        }

        for (uint32_t i = 0; i < 6; ++i) {
            // Begin of the render pass
            {
                // All the pipelines have the same renderPass
                const API::RenderPass* renderPass = irradianceMapPipeline->getPipelineAPI().getRenderPass();

                API::CommandBuffer::CmdBeginRenderPass beginRenderPass{
                    /* beginRenderPass.framebuffer  */ framebuffer,
                    /* beginRenderPass.renderArea   */ {},
                    /* beginRenderPass.clearValues  */ {}
                };

                beginRenderPass.renderArea.offset = {0, 0};
                beginRenderPass.renderArea.extent = {irradianceMapSize, irradianceMapSize};

                beginRenderPass.clearValues.resize(2);
                beginRenderPass.clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
                beginRenderPass.clearValues[1].depthStencil = {1.0f, 0};

                cmdBuffer.beginRenderPass(*renderPass, beginRenderPass);

                const VkViewport vkViewport{
                    /* vkViewport.x         */ 0.0f,
                    /* vkViewport.y         */ 0.0f,
                    /* vkViewport.width     */ static_cast<float>(irradianceMapSize),
                    /* vkViewport.height    */ static_cast<float>(irradianceMapSize),
                    /* vkViewport.minDepth  */ 0.0f,
                    /* vkViewport.maxDepth  */ 1.0f,
                };

                const VkRect2D scissor{
                    /* scissor.offset */ {
                        0,
                        0
                    },
                    /* scissor.extent */ {
                       static_cast<uint32_t>(irradianceMapSize),
                       static_cast<uint32_t>(irradianceMapSize)
                    }
                };

                cmdBuffer.setViewport({vkViewport});
                cmdBuffer.setScissor({scissor});
            }

            const Math::Mat4x4f pushConstants[] = {
                projection * matrices[i]
            };

            const API::CommandBuffer::CmdPushConstants cmdPushConstants{
                /* cmdPushConstants.layout      */ static_cast<VkPipelineLayout>(*irradianceMapPipeline->getPipelineAPI().getLayout()),
                /* cmdPushConstants.stageFlags  */ VK_SHADER_STAGE_VERTEX_BIT,
                /* cmdPushConstants.offset      */ 0,
                /* cmdPushConstants.size        */ sizeof(pushConstants),
                /* cmdPushConstants.values      */ pushConstants
            };

            cmdBuffer.pushConstants(cmdPushConstants);

            cmdBuffer.drawIndexed(cmdDrawIndexed);

            // End of the render pass
            cmdBuffer.endRenderPass();

            // Change offscreen image layout to VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL for render
            {
                API::CommandBuffer::CmdPipelineBarrier::ImageMemoryBarrier imageMemoryBarrier;
                imageMemoryBarrier.image = &offscreenImage;
                imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                API::CommandBuffer::CmdPipelineBarrier cmdPipelineBarrier;
                cmdPipelineBarrier.imageMemoryBarriers.push_back(std::move(imageMemoryBarrier));
                cmdBuffer.pipelineBarrier(cmdPipelineBarrier);
            }

            // Copy region for transfer from framebuffer to cube face
            VkImageCopy copyRegion = {};

            copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.srcSubresource.baseArrayLayer = 0;
            copyRegion.srcSubresource.mipLevel = 0;
            copyRegion.srcSubresource.layerCount = 1;
            copyRegion.srcOffset = { 0, 0, 0 };

            copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.dstSubresource.baseArrayLayer = i;
            copyRegion.dstSubresource.mipLevel = 0;
            copyRegion.dstSubresource.layerCount = 1;
            copyRegion.dstOffset = { 0, 0, 0 };

            copyRegion.extent.width = irradianceMapSize;
            copyRegion.extent.height = irradianceMapSize;
            copyRegion.extent.depth = 1;

            const API::CommandBuffer::CmdCopyImage cmdCopyImage{
                /* cmdCopyImage.srcImage      */ offscreenImage,
                /* cmdCopyImage.srcImageLayout  */ VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                /* cmdCopyImage.dstImage      */ Resource::SharedPtr<Render::Texture>::cast(irradianceMap->_environnementTexture)->getImage(),
                /* cmdCopyImage.dsrImageLayout        */ VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                /* cmdCopyImage.regions      */ { copyRegion }
            };

            cmdBuffer.copyImage(cmdCopyImage);

            // Change offscreen image layout to VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL for render
            {
                API::CommandBuffer::CmdPipelineBarrier::ImageMemoryBarrier imageMemoryBarrier;
                imageMemoryBarrier.image = &offscreenImage;
                imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                API::CommandBuffer::CmdPipelineBarrier cmdPipelineBarrier;
                cmdPipelineBarrier.imageMemoryBarriers.push_back(std::move(imageMemoryBarrier));
                cmdBuffer.pipelineBarrier(cmdPipelineBarrier);
            }
        }

        // Change irradiance layout to VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL for copy
        {
            API::CommandBuffer::CmdPipelineBarrier pipelineBarrier;
            pipelineBarrier.imageMemoryBarriers.resize(1);
            pipelineBarrier.imageMemoryBarriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            pipelineBarrier.imageMemoryBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            pipelineBarrier.imageMemoryBarriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            pipelineBarrier.imageMemoryBarriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            pipelineBarrier.imageMemoryBarriers[0].image = &Resource::SharedPtr<Render::Texture>::cast(irradianceMap->_environnementTexture)->getImage();
            pipelineBarrier.imageMemoryBarriers[0].subresourceRange.layerCount = 6;

            cmdBuffer.pipelineBarrier(pipelineBarrier);
        }

        if (!cmdBuffer.end()) {
            return nullptr;
        }
    }

    if (!graphicsQueue->submit(
        cmdBuffer,
        {},
        {},
        {},
        static_cast<VkFence>(fence)
    )) {
        LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createIrradianceMap: Can't submit work to graphics queue: {}", result);
        return nullptr;
    }

    if (!fence.wait() || !graphicsQueue->waitIdle()) {
        LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createIrradianceMap:: Can't wait fence");
        return nullptr;
    }

    cmdBuffer.destroy();
    commandPool.destroy();
    offscreenImage.destroy();
    offscreenImageView.destroy();
    framebuffer.destroy();
    descriptorPool.destroy();
    fence.destroy();

    return vkRenderer.getResourceManager()->add<::lug::Graphics::Render::SkyBox>(std::move(resource));
}

Resource::SharedPtr<lug::Graphics::Render::SkyBox> SkyBox::createPrefilteredMap(lug::Graphics::Renderer& renderer) const {
    constexpr uint32_t prefilteredMapSize = 512;
    const uint32_t mipMapCount = static_cast<uint32_t>(floor(log2(prefilteredMapSize))) + 1;

    // Constructor of SkyBox is private, we can't use std::make_unique
    std::unique_ptr<Resource> resource{new Vulkan::Render::SkyBox(_name + "_prefiltered_map")};
    Vulkan::Render::SkyBox* prefilteredMap = static_cast<Vulkan::Render::SkyBox*>(resource.get());

    Vulkan::Renderer& vkRenderer = static_cast<Vulkan::Renderer&>(renderer);

    auto prefilteredMapPipeline = vkRenderer.getPipeline(Render::Pipeline::getPrefilteredMapBaseId());

    if (!prefilteredMapPipeline) {
        LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::createPrefilteredMap: Can't get the prefiltered map pipeline");
        return nullptr;
    }

    lug::Graphics::Builder::Texture textureBuilder(vkRenderer);

    const Resource::SharedPtr<lug::Graphics::Vulkan::Render::Texture> texture = Resource::SharedPtr<Render::Texture>::cast(getEnvironnementTexture());
    if (!texture) {
        LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::createPrefilteredMap: The skybox doesn't have an environnement");
        return nullptr;
    }

    textureBuilder.setType(lug::Graphics::Builder::Texture::Type::CubeMap);
    textureBuilder.setMipLevels(mipMapCount);
    textureBuilder.setMagFilter(texture->getMagFilter());
    textureBuilder.setMinFilter(texture->getMinFilter());
    textureBuilder.setMipMapFilter(texture->getMipMapFilter());
    textureBuilder.setWrapS(texture->getWrapS());
    textureBuilder.setWrapT(texture->getWrapT());

    // TODO: Check which format to use
    if (!textureBuilder.addLayer(prefilteredMapSize, prefilteredMapSize, lug::Graphics::Render::Texture::Format::R32G32B32A32_SFLOAT)
        || !textureBuilder.addLayer(prefilteredMapSize, prefilteredMapSize, lug::Graphics::Render::Texture::Format::R32G32B32A32_SFLOAT)
        || !textureBuilder.addLayer(prefilteredMapSize, prefilteredMapSize, lug::Graphics::Render::Texture::Format::R32G32B32A32_SFLOAT)
        || !textureBuilder.addLayer(prefilteredMapSize, prefilteredMapSize, lug::Graphics::Render::Texture::Format::R32G32B32A32_SFLOAT)
        || !textureBuilder.addLayer(prefilteredMapSize, prefilteredMapSize, lug::Graphics::Render::Texture::Format::R32G32B32A32_SFLOAT)
        || !textureBuilder.addLayer(prefilteredMapSize, prefilteredMapSize, lug::Graphics::Render::Texture::Format::R32G32B32A32_SFLOAT)) {
        LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::build: Can't create prefiltered map texture layers");
        return nullptr;
    }

    prefilteredMap->_environnementTexture = textureBuilder.build();
    if (!prefilteredMap->_environnementTexture) {
        LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::build Can't create the prefiltered map texture");
        return nullptr;
    }

    // Create Framebuffer for prefiltered map generation
    API::CommandPool commandPool;
    API::CommandBuffer cmdBuffer;
    API::Image offscreenImage;
    API::ImageView offscreenImageView;
    API::DeviceMemory imagesMemory;
    API::Framebuffer framebuffer;
    API::Fence fence;
    API::DescriptorPool descriptorPool;
    API::DescriptorSet descriptorSet;
    const API::Queue* graphicsQueue = nullptr;
    VkResult result{VK_SUCCESS};
    {
        // Graphics queue
        graphicsQueue = vkRenderer.getDevice().getQueue("queue_graphics");
        if (!graphicsQueue) {
            LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::createPrefilteredMap: Can't find queue with name queue_graphics");
            return nullptr;
        }

        // Command pool
        API::Builder::CommandPool commandPoolBuilder(vkRenderer.getDevice(), *graphicsQueue->getQueueFamily());
        if (!commandPoolBuilder.build(commandPool, &result)) {
            LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createPrefilteredMap: Can't create the graphics command pool: {}", result);
            return nullptr;
        }

        // Command buffer
        API::Builder::CommandBuffer commandBufferBuilder(vkRenderer.getDevice(), commandPool);
        commandBufferBuilder.setLevel(VK_COMMAND_BUFFER_LEVEL_PRIMARY);

        // Create the render command buffer
        if (!commandBufferBuilder.build(cmdBuffer, &result)) {
            LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::createPrefilteredMap: Can't create the command buffer: {}", result);
            return nullptr;
        }

        // Descriptor pool
        {
            API::Builder::DescriptorPool descriptorPoolBuilder(vkRenderer.getDevice());

            descriptorPoolBuilder.setFlags(0);

            descriptorPoolBuilder.setMaxSets(42);

            std::vector<VkDescriptorPoolSize> poolSizes{
                {
                    /* poolSize.type            */ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    /* poolSize.descriptorCount */ 42
                }
            };
            descriptorPoolBuilder.setPoolSizes(poolSizes);

            if (!descriptorPoolBuilder.build(descriptorPool, &result)) {
                LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::createPrefilteredMap: Can't create the descriptor pool: {}", result);
                return nullptr;
            }
        }

        // Descriptor set
        {
            API::Builder::DescriptorSet descriptorSetBuilder(vkRenderer.getDevice(), descriptorPool);
            descriptorSetBuilder.setDescriptorSetLayouts({static_cast<VkDescriptorSetLayout>(prefilteredMapPipeline->getPipelineAPI().getLayout()->getDescriptorSetLayouts()[0])});

            if (!descriptorSetBuilder.build(descriptorSet, &result)) {
                LUG_LOG.error("DescriptorSetPool: Can't create descriptor set: {}", result);
                return nullptr;
            }

            descriptorSet.updateImages(
                0,
                0,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                {
                    {
                        /* sampler       */ static_cast<VkSampler>(texture->getSampler()),
                        /* imageView     */ static_cast<VkImageView>(texture->getImageView()),
                        /* imageLayout   */ VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    }
                }
            );
        }

        // Fence
        {
            API::Builder::Fence fenceBuilder(vkRenderer.getDevice());

            if (!fenceBuilder.build(fence, &result)) {
                LUG_LOG.error("Resource::SharedPtr<::lug::Graphics::Render::SkyBox>::createPrefilteredMap: Can't create render fence: {}", result);
                return nullptr;
            }
        }

        // Depth and offscreen images
        {
            API::Builder::DeviceMemory deviceMemoryBuilder(vkRenderer.getDevice());
            deviceMemoryBuilder.setMemoryFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            // Create image and add it to API::Builder::DeviceMemory
            const VkExtent3D extent{
                /* extent.width     */ prefilteredMapSize,
                /* extent.height    */ prefilteredMapSize,
                /* extent.depth     */ 1
            };

            // Create offscreen image
            {
                API::Builder::Image imageBuilder(vkRenderer.getDevice());

                imageBuilder.setExtent(extent);
                imageBuilder.setUsage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                imageBuilder.setPreferedFormats({ Resource::SharedPtr<Render::Texture>::cast(prefilteredMap->_environnementTexture)->getImage().getFormat() });
                imageBuilder.setQueueFamilyIndices({ graphicsQueue->getQueueFamily()->getIdx() });
                imageBuilder.setTiling(VK_IMAGE_TILING_OPTIMAL);

                if (!imageBuilder.build(offscreenImage, &result)) {
                    LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createPrefilteredMap: Can't create depth buffer image: {}", result);
                    return nullptr;
                }

                if (!deviceMemoryBuilder.addImage(offscreenImage)) {
                    LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createPrefilteredMap: Can't add image to device memory");
                    return nullptr;
                }
            }

            // Initialize depth buffer memory
            {
                if (!deviceMemoryBuilder.build(imagesMemory, &result)) {
                    LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createPrefilteredMap: Can't create device memory: {}", result);
                    return nullptr;
                }
            }

            // Create offscreen image view
            {
                API::Builder::ImageView imageViewBuilder(vkRenderer.getDevice(), offscreenImage);

                imageViewBuilder.setFormat(offscreenImage.getFormat());
                imageViewBuilder.setAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);

                if (!imageViewBuilder.build(offscreenImageView, &result)) {
                    LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createPrefilteredMap: Can't create depth buffer image view: {}", result);
                    return nullptr;
                }
            }
        }

        // Create framebuffer
        {
            // Create depth buffer image view
            API::Builder::Framebuffer framebufferBuilder(vkRenderer.getDevice());

            const API::RenderPass* renderPass = prefilteredMapPipeline->getPipelineAPI().getRenderPass();

            framebufferBuilder.setRenderPass(renderPass);
            framebufferBuilder.addAttachment(&offscreenImageView);
            framebufferBuilder.setWidth(prefilteredMapSize);
            framebufferBuilder.setHeight(prefilteredMapSize);

            if (!framebufferBuilder.build(framebuffer, &result)) {
                LUG_LOG.error("Forward::initFramebuffers: Can't create framebuffer: {}", result);
                return nullptr;
            }
        }
    }

    // Generate the prefiltered cube map
    {
        if (!cmdBuffer.begin()) {
            return nullptr;
        }

        cmdBuffer.bindPipeline(prefilteredMapPipeline->getPipelineAPI());

        // Bind descriptor set of the skybox
        {
            const API::CommandBuffer::CmdBindDescriptors skyBoxBind{
                /* skyBoxBind.pipelineLayout     */ *prefilteredMapPipeline->getPipelineAPI().getLayout(),
                /* skyBoxBind.pipelineBindPoint  */ VK_PIPELINE_BIND_POINT_GRAPHICS,
                /* skyBoxBind.firstSet           */ 0,
                /* skyBoxBind.descriptorSets     */ {&descriptorSet},
                /* skyBoxBind.dynamicOffsets     */ {},
            };

            cmdBuffer.bindDescriptorSets(skyBoxBind);
        }

        auto& primitiveSet = SkyBox::getMesh()->getPrimitiveSets()[0];

        cmdBuffer.bindVertexBuffers(
            {static_cast<API::Buffer*>(primitiveSet.position->_data)},
            {0}
        );

        API::Buffer* indicesBuffer = static_cast<API::Buffer*>(primitiveSet.indices->_data);
        cmdBuffer.bindIndexBuffer(*indicesBuffer, VK_INDEX_TYPE_UINT16);
        const API::CommandBuffer::CmdDrawIndexed cmdDrawIndexed {
            /* cmdDrawIndexed.indexCount    */ primitiveSet.indices->buffer.elementsCount,
            /* cmdDrawIndexed.instanceCount */ 1,
        };

        Math::Mat4x4f projection = Math::Geometry::perspective(Math::halfPi<float>(), 1.0f, 0.1f, static_cast<float>(prefilteredMapSize));
        std::vector<Math::Mat4x4f> matrices = {
            Math::Geometry::lookAt<float>({0.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}),
            Math::Geometry::lookAt<float>({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}),
            Math::Geometry::lookAt<float>({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}),
            Math::Geometry::lookAt<float>({0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}),
            Math::Geometry::lookAt<float>({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}),
            Math::Geometry::lookAt<float>({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f})
        };

        // Change offscreen image layout to VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL for render
        {
            API::CommandBuffer::CmdPipelineBarrier pipelineBarrier;
            pipelineBarrier.imageMemoryBarriers.resize(1);
            pipelineBarrier.imageMemoryBarriers[0].srcAccessMask = 0;
            pipelineBarrier.imageMemoryBarriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            pipelineBarrier.imageMemoryBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            pipelineBarrier.imageMemoryBarriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            pipelineBarrier.imageMemoryBarriers[0].image = &offscreenImage;

            cmdBuffer.pipelineBarrier(pipelineBarrier);
        }

        // Change prefiltered layout to VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL for copy
        {
            API::CommandBuffer::CmdPipelineBarrier pipelineBarrier;
            pipelineBarrier.imageMemoryBarriers.resize(1);
            pipelineBarrier.imageMemoryBarriers[0].srcAccessMask = 0;
            pipelineBarrier.imageMemoryBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            pipelineBarrier.imageMemoryBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            pipelineBarrier.imageMemoryBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            pipelineBarrier.imageMemoryBarriers[0].image = &Resource::SharedPtr<Render::Texture>::cast(prefilteredMap->_environnementTexture)->getImage();
            pipelineBarrier.imageMemoryBarriers[0].subresourceRange.levelCount = mipMapCount;
            pipelineBarrier.imageMemoryBarriers[0].subresourceRange.layerCount = 6;

            cmdBuffer.pipelineBarrier(pipelineBarrier);
        }


        for (uint32_t m = 0; m < mipMapCount; m++) {
            const float roughness = (float)m / (float)(mipMapCount - 1);

            for (uint32_t i = 0; i < 6; ++i) {
                // Begin of the render pass
                {
                    // All the pipelines have the same renderPass
                    const API::RenderPass* renderPass = prefilteredMapPipeline->getPipelineAPI().getRenderPass();

                    API::CommandBuffer::CmdBeginRenderPass beginRenderPass{
                        /* beginRenderPass.framebuffer  */ framebuffer,
                        /* beginRenderPass.renderArea   */ {},
                        /* beginRenderPass.clearValues  */ {}
                    };

                    beginRenderPass.renderArea.offset = {0, 0};
                    beginRenderPass.renderArea.extent = {prefilteredMapSize, prefilteredMapSize};

                    beginRenderPass.clearValues.resize(2);
                    beginRenderPass.clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
                    beginRenderPass.clearValues[1].depthStencil = {1.0f, 0};

                    cmdBuffer.beginRenderPass(*renderPass, beginRenderPass);

                    const VkViewport vkViewport{
                        /* vkViewport.x         */ 0.0f,
                        /* vkViewport.y         */ 0.0f,
                        /* vkViewport.width     */ static_cast<float>(prefilteredMapSize * std::pow(0.5f, m)),
                        /* vkViewport.height    */ static_cast<float>(prefilteredMapSize * std::pow(0.5f, m)),
                        /* vkViewport.minDepth  */ 0.0f,
                        /* vkViewport.maxDepth  */ 1.0f,
                    };

                    const VkRect2D scissor{
                        /* scissor.offset */ {
                            0,
                            0
                        },
                        /* scissor.extent */ {
                           static_cast<uint32_t>(prefilteredMapSize),
                           static_cast<uint32_t>(prefilteredMapSize)
                        }
                    };

                    cmdBuffer.setViewport({vkViewport});
                    cmdBuffer.setScissor({scissor});
                }

                {
                    const Math::Mat4x4f pushConstants[] = {
                        projection * matrices[i]
                    };

                    const API::CommandBuffer::CmdPushConstants cmdPushConstants{
                        /* cmdPushConstants.layout      */ static_cast<VkPipelineLayout>(*prefilteredMapPipeline->getPipelineAPI().getLayout()),
                        /* cmdPushConstants.stageFlags  */ VK_SHADER_STAGE_VERTEX_BIT,
                        /* cmdPushConstants.offset      */ 0,
                        /* cmdPushConstants.size        */ sizeof(pushConstants),
                        /* cmdPushConstants.values      */ pushConstants
                    };

                    cmdBuffer.pushConstants(cmdPushConstants);
                }

                {
                    const API::CommandBuffer::CmdPushConstants cmdPushConstants{
                        /* cmdPushConstants.layout      */ static_cast<VkPipelineLayout>(*prefilteredMapPipeline->getPipelineAPI().getLayout()),
                        /* cmdPushConstants.stageFlags  */ VK_SHADER_STAGE_FRAGMENT_BIT,
                        /* cmdPushConstants.offset      */ sizeof(Math::Mat4x4f),
                        /* cmdPushConstants.size        */ sizeof(roughness),
                        /* cmdPushConstants.values      */ &roughness
                    };

                    cmdBuffer.pushConstants(cmdPushConstants);
                }

                cmdBuffer.drawIndexed(cmdDrawIndexed);

                // End of the render pass
                cmdBuffer.endRenderPass();

                // Change offscreen image layout to VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL for render
                {
                    API::CommandBuffer::CmdPipelineBarrier::ImageMemoryBarrier imageMemoryBarrier;
                    imageMemoryBarrier.image = &offscreenImage;
                    imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    API::CommandBuffer::CmdPipelineBarrier cmdPipelineBarrier;
                    cmdPipelineBarrier.imageMemoryBarriers.push_back(std::move(imageMemoryBarrier));
                    cmdBuffer.pipelineBarrier(cmdPipelineBarrier);
                }

                // Copy region for transfer from framebuffer to cube face
                VkImageCopy copyRegion = {};

                copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                copyRegion.srcSubresource.baseArrayLayer = 0;
                copyRegion.srcSubresource.mipLevel = 0;
                copyRegion.srcSubresource.layerCount = 1;
                copyRegion.srcOffset = { 0, 0, 0 };

                copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                copyRegion.dstSubresource.baseArrayLayer = i;
                copyRegion.dstSubresource.mipLevel = m;
                copyRegion.dstSubresource.layerCount = 1;
                copyRegion.dstOffset = { 0, 0, 0 };

                copyRegion.extent.width = static_cast<uint32_t>(prefilteredMapSize * std::pow(0.5f, m));
                copyRegion.extent.height = static_cast<uint32_t>(prefilteredMapSize * std::pow(0.5f, m));
                copyRegion.extent.depth = 1;

                const API::CommandBuffer::CmdCopyImage cmdCopyImage{
                    /* cmdCopyImage.srcImage      */ offscreenImage,
                    /* cmdCopyImage.srcImageLayout  */ VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    /* cmdCopyImage.dstImage      */ Resource::SharedPtr<Render::Texture>::cast(prefilteredMap->_environnementTexture)->getImage(),
                    /* cmdCopyImage.dsrImageLayout        */ VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    /* cmdCopyImage.regions      */ { copyRegion }
                };

                cmdBuffer.copyImage(cmdCopyImage);

                // Change offscreen image layout to VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL for render
                {
                    API::CommandBuffer::CmdPipelineBarrier::ImageMemoryBarrier imageMemoryBarrier;
                    imageMemoryBarrier.image = &offscreenImage;
                    imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    API::CommandBuffer::CmdPipelineBarrier cmdPipelineBarrier;
                    cmdPipelineBarrier.imageMemoryBarriers.push_back(std::move(imageMemoryBarrier));
                    cmdBuffer.pipelineBarrier(cmdPipelineBarrier);
                }
            }
        }

        // Change prefiltered layout to VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL for copy
        {
            API::CommandBuffer::CmdPipelineBarrier pipelineBarrier;
            pipelineBarrier.imageMemoryBarriers.resize(1);
            pipelineBarrier.imageMemoryBarriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            pipelineBarrier.imageMemoryBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            pipelineBarrier.imageMemoryBarriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            pipelineBarrier.imageMemoryBarriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            pipelineBarrier.imageMemoryBarriers[0].image = &Resource::SharedPtr<Render::Texture>::cast(prefilteredMap->_environnementTexture)->getImage();
            pipelineBarrier.imageMemoryBarriers[0].subresourceRange.levelCount = mipMapCount;
            pipelineBarrier.imageMemoryBarriers[0].subresourceRange.layerCount = 6;

            cmdBuffer.pipelineBarrier(pipelineBarrier);
        }

        if (!cmdBuffer.end()) {
            return nullptr;
        }
    }

    if (!graphicsQueue->submit(
        cmdBuffer,
        {},
        {},
        {},
        static_cast<VkFence>(fence)
    )) {
        LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createPrefilteredMap: Can't submit work to graphics queue: {}", result);
        return nullptr;
    }

    if (!fence.wait() || !graphicsQueue->waitIdle()) {
        LUG_LOG.error("Forward::iniResource::SharedPtr<::lug::Graphics::Render::SkyBox>::createPrefilteredMap:: Can't wait fence");
        return nullptr;
    }

    cmdBuffer.destroy();
    commandPool.destroy();
    offscreenImage.destroy();
    offscreenImageView.destroy();
    framebuffer.destroy();
    descriptorPool.destroy();
    fence.destroy();

    return vkRenderer.getResourceManager()->add<::lug::Graphics::Render::SkyBox>(std::move(resource));
}

} // Render
} // Vulkan
} // Graphics
} // lug
