#include "src/oxpch.h"
#include "VulkanRenderer.h"

#include <future>

#include "VulkanCommandBuffer.h"
#include "VulkanContext.h"
#include "VulkanPipeline.h"
#include "VulkanSwapchain.h"
#include "Utils/VulkanUtils.h"
#include "Core/Resources.h"
#include "Render/Mesh.h"
#include "Render/Window.h"
#include "Render/PBR/Prefilter.h"
#include "Render/Vulkan/VulkanBuffer.h"
#include "Render/ResourcePool.h"
#include "Render/ShaderLibrary.h"
#include "Utils/Profiler.h"
#include "Core/Entity.h"

#include <backends/imgui_impl_vulkan.h>

namespace Oxylus {
  VulkanSwapchain VulkanRenderer::SwapChain;
  VulkanRenderer::RendererContext VulkanRenderer::s_RendererContext;
  VulkanRenderer::RendererData VulkanRenderer::s_RendererData;
  VulkanRenderer::RendererResources VulkanRenderer::s_Resources;
  VulkanRenderer::Pipelines VulkanRenderer::s_Pipelines;
  VulkanRenderer::FrameBuffers VulkanRenderer::s_FrameBuffers;
  RendererConfig VulkanRenderer::s_RendererConfig;

  static VulkanDescriptorSet s_PostProcessDescriptorSet;
  static VulkanDescriptorSet s_SkyboxDescriptorSet;
  static VulkanDescriptorSet s_ComputeDescriptorSet;
  static VulkanDescriptorSet s_SSAODescriptorSet;
  static VulkanDescriptorSet s_SSAOBlurDescriptorSet;
  static VulkanDescriptorSet s_SSRDescriptorSet;
  static VulkanDescriptorSet s_QuadDescriptorSet;
  static VulkanDescriptorSet s_DepthDescriptorSet;
  static VulkanDescriptorSet s_ShadowDepthDescriptorSet;
  static VulkanDescriptorSet s_BloomDescriptorSet;
  static VulkanDescriptorSet s_CompositeDescriptorSet;
  static VulkanDescriptorSet s_AtmosphereDescriptorSet;
  static VulkanDescriptorSet s_DepthOfFieldDescriptorSet;
  static Mesh s_SkyboxCube;
  static VulkanBuffer s_TriangleVertexBuffer;
  static VulkanBuffer s_QuadVertexBuffer;

  std::vector<VulkanRenderer::MeshData> VulkanRenderer::s_MeshDrawList;
  static bool s_ForceUpdateMaterials = false;

  std::vector<Entity> VulkanRenderer::s_SceneLights;
  Entity VulkanRenderer::s_Skylight;
  std::vector<VulkanRenderer::LightingData> VulkanRenderer::s_PointLightsData;

  std::vector<VulkanRenderer::QuadData> VulkanRenderer::s_QuadDrawList;
  std::vector<VulkanRenderer::RendererData::Vertex> VulkanRenderer::s_QuadVertexDataBuffer;

  struct LightChangeEvent {};

  static EventDispatcher s_LightBufferDispatcher;

  /*
    Calculate frustum split depths and matrices for the shadow map cascades
    Based on https://johanmedestrom.wordpress.com/2016/03/18/opengl-cascaded-shadow-maps/
  */
  void VulkanRenderer::UpdateCascades(const glm::mat4& Transform,
                                      Camera* camera,
                                      RendererData::DirectShadowUB& cascadesUbo) {
    ZoneScoped;
    float cascadeSplits[SHADOW_MAP_CASCADE_COUNT]{};

    float nearClip = camera->NearClip;
    float farClip = camera->FarClip;
    float clipRange = farClip - nearClip;

    float minZ = nearClip;
    float maxZ = nearClip + clipRange;

    float range = maxZ - minZ;
    float ratio = maxZ / minZ;

    constexpr float cascadeSplitLambda = 0.95f;
    // Calculate split depths based on view camera frustum
    // Based on method presented in https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch10.html
    for (uint32_t i = 0; i < SHADOW_MAP_CASCADE_COUNT; i++) {
      float p = (float)(i + 1) / static_cast<float>(SHADOW_MAP_CASCADE_COUNT);
      float log = minZ * std::pow(ratio, p);
      float uniform = minZ + range * p;
      float d = cascadeSplitLambda * (log - uniform) + uniform;
      cascadeSplits[i] = (d - nearClip) / clipRange;
    }

    // Calculate orthographic projection matrix for each cascade
    float lastSplitDist = 0.0;
    for (uint32_t i = 0; i < SHADOW_MAP_CASCADE_COUNT; i++) {
      float splitDist = cascadeSplits[i];

      Vec3 frustumCorners[8] = {
        Vec3(-1.0f, 1.0f, 0.0f), Vec3(1.0f, 1.0f, 0.0f), Vec3(1.0f, -1.0f, 0.0f),
        Vec3(-1.0f, -1.0f, 0.0f), Vec3(-1.0f, 1.0f, 1.0f), Vec3(1.0f, 1.0f, 1.0f),
        Vec3(1.0f, -1.0f, 1.0f), Vec3(-1.0f, -1.0f, 1.0f),
      };

      // Project frustum corners into world space
      Mat4 invCam = inverse(camera->GetProjectionMatrix() * camera->GetViewMatrix());
      for (auto& frustumCorner : frustumCorners) {
        Vec4 invCorner = invCam * Vec4(frustumCorner, 1.0f);
        frustumCorner = invCorner / invCorner.w;
      }

      for (uint32_t j = 0; j < 4; j++) {
        Vec3 dist = frustumCorners[j + 4] - frustumCorners[j];
        frustumCorners[j + 4] = frustumCorners[j] + dist * splitDist;
        frustumCorners[j] = frustumCorners[j] + dist * lastSplitDist;
      }

      // Get frustum center
      auto frustumCenter = Vec3(0.0f);
      for (auto& frustumCorner : frustumCorners) {
        frustumCenter += frustumCorner;
      }
      frustumCenter /= 8.0f;

      float radius = 0.0f;
      for (auto& frustumCorner : frustumCorners) {
        float distance = glm::length(frustumCorner - frustumCenter);
        radius = glm::max(radius, distance);
      }
      radius = std::ceil(radius * 16.0f) / 16.0f;

      auto maxExtents = Vec3(radius);
      Vec3 minExtents = -maxExtents;

      Vec4 zDir = Transform * Vec4(0, 0, 1, 0);
      //glm::vec3 pos = Transform[3];
      Vec3 dir = glm::normalize(Vec3(zDir));
      //glm::vec3 lookAt = pos + dir;
      Mat4 lightViewMatrix = glm::lookAt(frustumCenter - dir * -minExtents.z,
        frustumCenter,
        Vec3(0.0f, 1.0f, 0.0f));
      Mat4 lightOrthoMatrix = glm::ortho(minExtents.x,
        maxExtents.x,
        minExtents.y,
        maxExtents.y,
        (maxExtents.z - minExtents.z) * -1,
        maxExtents.z - minExtents.z);

      //glm::vec4 zDir = Transform * glm::vec4(0, 0, 1, 0);
      //float near_plane = -100.0f, far_plane = 100.0f;
      //glm::mat4 lightProjection = glm::ortho(-20.0f, 20.0f, -10.0f, 10.0f, near_plane, far_plane);
      //glm::vec3 pos = Transform[3];
      //glm::vec3 dir = glm::normalize(glm::vec3(zDir));
      //glm::vec3 lookAt = pos + dir;
      //glm::mat4 dirLightView = glm::lookAt(pos, lookAt, glm::vec3(0, 1, 0));
      //glm::mat4 dirLightViewProj = lightProjection * dirLightView;

      // Store split distance and matrix in cascade
      cascadesUbo.cascadeSplits[i] = (camera->NearClip + splitDist * clipRange) * -1.0f;
      cascadesUbo.cascadeViewProjMat[i] = lightOrthoMatrix * lightViewMatrix;
      //cascadesUbo.cascadeViewProjMat[i] = dirLightViewProj;

      lastSplitDist = cascadeSplits[i];
    }
  }

  void VulkanRenderer::UpdateLightingData() {
    ZoneScoped;
    for (auto& e : s_SceneLights) {
      auto& lightComponent = e.GetComponent<LightComponent>();
      auto& transformComponent = e.GetComponent<TransformComponent>();
      switch (lightComponent.Type) {
        case LightComponent::LightType::Directional: break;
        case LightComponent::LightType::Point: {
          s_PointLightsData.emplace_back(LightingData{
            Vec4{transformComponent.Translation, lightComponent.Intensity},
            Vec4{lightComponent.Color, lightComponent.Range}, Vec4{transformComponent.Rotation, 1.0f}
          });
        }
        break;
        case LightComponent::LightType::Spot: break;
      }
    }

    if (!s_PointLightsData.empty()) {
      s_RendererData.LightsBuffer.Copy(s_PointLightsData);
      s_PointLightsData.clear();
    }
  }

  void VulkanRenderer::UpdateUniformBuffers() {
    ZoneScoped;
    s_RendererData.UBO_VS.projection = s_RendererContext.CurrentCamera->GetProjectionMatrixFlipped();
    s_RendererData.SkyboxBuffer.Copy(&s_RendererData.UBO_VS, sizeof s_RendererData.UBO_VS);

    s_RendererData.UBO_VS.projection = s_RendererContext.CurrentCamera->GetProjectionMatrixFlipped();
    s_RendererData.UBO_VS.view = s_RendererContext.CurrentCamera->GetViewMatrix();
    s_RendererData.UBO_VS.camPos = s_RendererContext.CurrentCamera->GetPosition();
    s_RendererData.VSBuffer.Copy(&s_RendererData.UBO_VS, sizeof s_RendererData.UBO_VS);

    s_RendererData.UBO_PbrPassParams.numLights = 1;
    s_RendererData.UBO_PbrPassParams.numThreads = (glm::ivec2(Window::GetWidth(), Window::GetHeight()) + PIXELS_PER_TILE - 1) /
                                                  PIXELS_PER_TILE;
    s_RendererData.UBO_PbrPassParams.numThreadGroups = (s_RendererData.UBO_PbrPassParams.numThreads + TILES_PER_THREADGROUP - 1) /
                                                       TILES_PER_THREADGROUP;
    s_RendererData.UBO_PbrPassParams.screenDimensions = glm::ivec2(Window::GetWidth(), Window::GetHeight());

    s_RendererData.ParametersBuffer.Copy(&s_RendererData.UBO_PbrPassParams, sizeof s_RendererData.UBO_PbrPassParams);

    s_RendererData.UBO_Atmosphere.LightPos = Vec4{
      Vec3(0.0f, glm::sin(glm::radians(s_RendererData.UBO_Atmosphere.Time * 360.0f)), glm::cos(glm::radians(s_RendererData.UBO_Atmosphere.Time * 360.0f))) * 149600000e3f,
      s_RendererData.UBO_Atmosphere.LightPos.w
    };
    s_RendererData.UBO_Atmosphere.InvProjection = glm::inverse(s_RendererContext.CurrentCamera->GetProjectionMatrixFlipped());
    s_RendererData.AtmosphereBuffer.Copy(&s_RendererData.UBO_Atmosphere, sizeof s_RendererData.UBO_Atmosphere);
  }

  void VulkanRenderer::GeneratePrefilter() {
    ZoneScoped;
    Prefilter::GenerateBRDFLUT(s_Resources.LutBRDF);
    Prefilter::GenerateIrradianceCube(s_Resources.IrradianceCube,
      s_SkyboxCube,
      VertexLayout({VertexComponent::POSITION, VertexComponent::NORMAL, VertexComponent::UV}),
      s_Resources.CubeMap.GetDescImageInfo());
    Prefilter::GeneratePrefilteredCube(s_Resources.PrefilteredCube,
      s_SkyboxCube,
      VertexLayout({VertexComponent::POSITION, VertexComponent::NORMAL, VertexComponent::UV}),
      s_Resources.CubeMap.GetDescImageInfo());
  }

  void VulkanRenderer::CreateGraphicsPipelines() {
    ZoneScoped;
    // Create shaders
    auto skyboxShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .VertexPath = Resources::GetResourcesPath("Shaders/Skybox.vert").string(),
      .FragmentPath = Resources::GetResourcesPath("Shaders/Skybox.frag").string(),
      .EntryPoint = "main", .Name = "Skybox"
    });
    auto pbrShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .VertexPath = Resources::GetResourcesPath("Shaders/PBRTiled.vert").string(),
      .FragmentPath = Resources::GetResourcesPath("Shaders/PBRTiled.frag").string(),
      .EntryPoint = "main", .Name = "PBRTiled",
    });
    auto unlitShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .VertexPath = Resources::GetResourcesPath("Shaders/Unlit.vert").string(),
      .FragmentPath = Resources::GetResourcesPath("Shaders/Unlit.frag").string(),
      .EntryPoint = "main", .Name = "Unlit",
    });
    auto directShadowShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .VertexPath = Resources::GetResourcesPath("Shaders/DirectShadowDepthPass.vert").string(),
      .FragmentPath = Resources::GetResourcesPath("Shaders/DirectShadowDepthPass.frag").string(),
      .EntryPoint = "main", .Name = "DirectShadowDepth"
    });
    auto depthPassShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .VertexPath = Resources::GetResourcesPath("Shaders/DepthNormalPass.vert").string(),
      .FragmentPath = Resources::GetResourcesPath("Shaders/DepthNormalPass.frag").string(),
      .EntryPoint = "main", .Name = "DepthPass"
    });
    auto ssaoShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .EntryPoint = "main",
      .Name = "SSAO",
      .ComputePath = Resources::GetResourcesPath("Shaders/SSAO.comp").string(),
    });
    auto bloomShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .EntryPoint = "main",
      .Name = "Bloom",
      .ComputePath = Resources::GetResourcesPath("Shaders/Bloom.comp").string(),
    });
    auto ssrShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .EntryPoint = "main",
      .Name = "SSR",
      .ComputePath = Resources::GetResourcesPath("Shaders/SSR.comp").string(),
    });
    auto atmosphereShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .EntryPoint = "main",
      .Name = "AtmosphereScattering",
      .ComputePath = Resources::GetResourcesPath("Shaders/AtmosphricScattering.comp").string(),
    });
    auto compositeShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .EntryPoint = "main",
      .Name = "Composite",
      .ComputePath = Resources::GetResourcesPath("Shaders/Composite.comp").string(),
    });
    auto postProcessShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .VertexPath = Resources::GetResourcesPath("Shaders/PostProcess.vert").string(),
      .FragmentPath = Resources::GetResourcesPath("Shaders/PostProcess.frag").string(),
      .EntryPoint = "main", .Name = "PostProcess"
    });
    auto quadShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .VertexPath = Resources::GetResourcesPath("Shaders/quad.vert").string(),
      .FragmentPath = Resources::GetResourcesPath("Shaders/quad.frag").string(),
      .EntryPoint = "main",
      .Name = "Quad",
    });
    auto frustumGridShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .EntryPoint = "main", .Name = "FrustumGrid",
      .ComputePath = Resources::GetResourcesPath("Shaders/ComputeFrustumGrid.comp").string(),
    });
    auto lightListShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .EntryPoint = "main", .Name = "LightList",
      .ComputePath = Resources::GetResourcesPath("Shaders/ComputeLightList.comp").string(),
    });
    auto uiShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .VertexPath = Resources::GetResourcesPath("Shaders/ui.vert").string(),
      .FragmentPath = Resources::GetResourcesPath("Shaders/ui.frag").string(),
      .EntryPoint = "main",
      .Name = "UI",
    });
    auto depthOfFieldShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .EntryPoint = "main",
      .Name = "DepthOfField",
      .ComputePath = Resources::GetResourcesPath("Shaders/DepthOfField.comp").string(),
    });
    auto gaussianBlurShader = ShaderLibrary::CreateShaderAsync(ShaderCI{
      .EntryPoint = "main",
      .Name = "GaussianBlur",
      .ComputePath = Resources::GetResourcesPath("Shaders/GaussianBlur.comp").string(),
    });
    PipelineDescription pipelineDescription{};
    pipelineDescription.Shader = skyboxShader.get();
    pipelineDescription.ColorAttachmentCount = 1;
    pipelineDescription.RenderTargets[0].Format = SwapChain.m_ImageFormat;
    pipelineDescription.RasterizerDesc.CullMode = vk::CullModeFlagBits::eNone;
    pipelineDescription.RasterizerDesc.DepthBias = false;
    pipelineDescription.RasterizerDesc.FrontCounterClockwise = true;
    pipelineDescription.RasterizerDesc.DepthClamppEnable = false;
    pipelineDescription.DepthSpec.DepthWriteEnable = false;
    pipelineDescription.DepthSpec.DepthReferenceAttachment = 1;
    pipelineDescription.DepthSpec.DepthEnable = true;
    pipelineDescription.DepthSpec.CompareOp = vk::CompareOp::eLessOrEqual;
    pipelineDescription.DepthSpec.FrontFace.StencilFunc = vk::CompareOp::eNever;
    pipelineDescription.DepthSpec.BackFace.StencilFunc = vk::CompareOp::eNever;
    pipelineDescription.DepthSpec.MinDepthBound = 0;
    pipelineDescription.DepthSpec.MaxDepthBound = 0;
    pipelineDescription.DepthSpec.DepthStenctilFormat = vk::Format::eD32Sfloat;
    pipelineDescription.DynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    pipelineDescription.VertexInputState = VertexInputDescription(VertexLayout({
      VertexComponent::POSITION, VertexComponent::NORMAL, VertexComponent::UV
    }));
    pipelineDescription.PushConstantRanges = {
      vk::PushConstantRange{vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4)}
    };
    pipelineDescription.SetDescriptions = {
      {
        SetDescription{0, 0, 1, vDT::eUniformBuffer, vSS::eVertex, nullptr, &s_RendererData.SkyboxBuffer.GetDescriptor()},
        SetDescription{1, 0, 1, vDT::eUniformBuffer, vSS::eFragment, nullptr, &s_RendererData.PostProcessBuffer.GetDescriptor()},
        SetDescription{6, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment, &s_Resources.CubeMap.GetDescImageInfo()},
      }
    };
    s_Pipelines.SkyboxPipeline.CreateGraphicsPipelineAsync(pipelineDescription).wait();

    std::vector<std::vector<SetDescription>> pbrDescriptorSet = {
      {
        SetDescription{0, 0, 1, vDT::eUniformBuffer, vSS::eFragment | vSS::eVertex, nullptr, &s_RendererData.VSBuffer.GetDescriptor()},
        SetDescription{1, 0, 1, vDT::eUniformBuffer, vSS::eFragment | vSS::eVertex, nullptr, &s_RendererData.ParametersBuffer.GetDescriptor()},
        SetDescription{2, 0, 1, vDT::eStorageBuffer, vSS::eFragment, nullptr, &s_RendererData.LightsBuffer.GetDescriptor()},
        SetDescription{3, 0, 1, vDT::eStorageBuffer, vSS::eFragment, nullptr, &s_RendererData.FrustumBuffer.GetDescriptor()},
        SetDescription{4, 0, 1, vDT::eStorageBuffer, vSS::eFragment, nullptr, &s_RendererData.LighIndexBuffer.GetDescriptor()},
        SetDescription{5, 0, 1, vDT::eStorageBuffer, vSS::eFragment, nullptr, &s_RendererData.LighGridBuffer.GetDescriptor()},
        SetDescription{6, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment, &s_Resources.IrradianceCube.GetDescImageInfo()},
        SetDescription{7, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment, &s_Resources.LutBRDF.GetDescImageInfo()},
        SetDescription{8, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment, &s_Resources.PrefilteredCube.GetDescImageInfo()},
        SetDescription{9, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment},
        SetDescription{10, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment},
        SetDescription{11, 0, 1, vDT::eUniformBuffer, vSS::eFragment, nullptr, &s_RendererData.DirectShadowBuffer.GetDescriptor()},
      },
      {
        SetDescription{0, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment},
        SetDescription{1, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment},
        SetDescription{2, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment},
        SetDescription{3, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment},
        SetDescription{4, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment},
      }
    };

    pipelineDescription.SetDescriptions = pbrDescriptorSet;
    pipelineDescription.PushConstantRanges.emplace_back(vk::ShaderStageFlagBits::eFragment,
      sizeof(glm::mat4),
      sizeof Material::Parameters);
    pipelineDescription.Shader = pbrShader.get();
    pipelineDescription.DepthSpec.DepthWriteEnable = true;
    pipelineDescription.DepthSpec.DepthEnable = true;
    pipelineDescription.RasterizerDesc.CullMode = vk::CullModeFlagBits::eBack;
    s_Pipelines.PBRPipeline.CreateGraphicsPipelineAsync(pipelineDescription).wait();

    PipelineDescription unlitPipelineDesc;
    unlitPipelineDesc.Shader = unlitShader.get();
    unlitPipelineDesc.ColorAttachmentCount = 1;
    unlitPipelineDesc.PushConstantRanges = {vk::PushConstantRange{vSS::eVertex, 0, sizeof glm::mat4() * 2}};
    unlitPipelineDesc.DepthSpec.DepthWriteEnable = false;
    unlitPipelineDesc.DepthSpec.DepthEnable = false;
    unlitPipelineDesc.RasterizerDesc.CullMode = vk::CullModeFlagBits::eBack;
    unlitPipelineDesc.VertexInputState = VertexInputDescription(VertexLayout({
      VertexComponent::POSITION, VertexComponent::NORMAL, VertexComponent::UV, VertexComponent::COLOR
    }));
    unlitPipelineDesc.BlendStateDesc.RenderTargets[0].BlendEnable = true;
    unlitPipelineDesc.BlendStateDesc.RenderTargets[0].DestBlend = vk::BlendFactor::eOneMinusSrcAlpha;

    s_Pipelines.UnlitPipeline.CreateGraphicsPipelineAsync(unlitPipelineDesc).wait();

    PipelineDescription depthpassdescription;
    depthpassdescription.Shader = depthPassShader.get();
    depthpassdescription.SetDescriptions = pbrDescriptorSet;
    depthpassdescription.DepthAttachmentFirst = true;
    depthpassdescription.DepthSpec.BoundTest = true;
    depthpassdescription.ColorAttachment = 1;
    depthpassdescription.SubpassDependencyCount = 2;
    depthpassdescription.SubpassDescription[0].SrcStageMask = vk::PipelineStageFlagBits::eBottomOfPipe;
    depthpassdescription.SubpassDescription[0].DstStageMask = vk::PipelineStageFlagBits::eEarlyFragmentTests;
    depthpassdescription.SubpassDescription[0].SrcAccessMask = vk::AccessFlagBits::eMemoryRead;
    depthpassdescription.SubpassDescription[0].DstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead |
                                                               vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    depthpassdescription.SubpassDescription[1].SrcSubpass = 0;
    depthpassdescription.SubpassDescription[1].DstSubpass = VK_SUBPASS_EXTERNAL;
    depthpassdescription.SubpassDescription[1].SrcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    depthpassdescription.SubpassDescription[1].DstStageMask = vk::PipelineStageFlagBits::eFragmentShader;
    depthpassdescription.SubpassDescription[1].SrcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    depthpassdescription.SubpassDescription[1].DstAccessMask = vk::AccessFlagBits::eShaderRead;
    depthpassdescription.DepthAttachmentLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    depthpassdescription.VertexInputState = VertexInputDescription(VertexLayout({
      VertexComponent::POSITION,
      VertexComponent::NORMAL,
      VertexComponent::UV,
      VertexComponent::TANGENT
    }));
    depthpassdescription.PushConstantRanges = {
      vk::PushConstantRange{vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4)}
    };
    depthpassdescription.PushConstantRanges.emplace_back(vk::ShaderStageFlagBits::eFragment,
      sizeof(glm::mat4),
      sizeof Material::Parameters);
    s_Pipelines.DepthPrePassPipeline.CreateGraphicsPipelineAsync(depthpassdescription).wait();

    pipelineDescription.Shader = directShadowShader.get();
    pipelineDescription.PushConstantRanges = {vk::PushConstantRange{vk::ShaderStageFlagBits::eVertex, 0, 68}};
    pipelineDescription.ColorAttachmentCount = 0;
    pipelineDescription.RasterizerDesc.CullMode = vk::CullModeFlagBits::eNone;
    pipelineDescription.DepthSpec.DepthEnable = true;
    pipelineDescription.RasterizerDesc.DepthClamppEnable = true;
    pipelineDescription.RasterizerDesc.FrontCounterClockwise = false;
    pipelineDescription.DepthSpec.BoundTest = false;
    pipelineDescription.DepthSpec.CompareOp = vk::CompareOp::eLessOrEqual;
    pipelineDescription.DepthSpec.MaxDepthBound = 0;
    pipelineDescription.DepthSpec.DepthReferenceAttachment = 0;
    pipelineDescription.DepthSpec.BackFace.StencilFunc = vk::CompareOp::eAlways;
    pipelineDescription.DepthAttachmentLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    pipelineDescription.SetDescriptions = {
      {
        SetDescription{0, 0, 1, vDT::eUniformBuffer, vSS::eVertex, nullptr, &s_RendererData.DirectShadowBuffer.GetDescriptor()}
      }
    };
    s_Pipelines.DirectShadowDepthPipeline.CreateGraphicsPipelineAsync(pipelineDescription).wait();

    PipelineDescription ssaoDescription;
    ssaoDescription.RenderTargets[0].Format = vk::Format::eR8Unorm;
    ssaoDescription.DepthSpec.DepthEnable = false;
    ssaoDescription.SubpassDescription[0].DstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    ssaoDescription.SubpassDescription[0].SrcAccessMask = {};
    ssaoDescription.SetDescriptions = {
      {
        SetDescription{0, 0, 1, vDT::eUniformBuffer, vSS::eCompute, nullptr, &s_RendererData.VSBuffer.GetDescriptor()},
        SetDescription{1, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
        SetDescription{2, 0, 1, vDT::eStorageImage, vSS::eCompute},
        SetDescription{3, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
      }
    };
    ssaoDescription.Shader = ssaoShader.get();
    s_Pipelines.SSAOPassPipeline.CreateComputePipelineAsync(ssaoDescription).wait();

    {
      PipelineDescription gaussianBlur;
      gaussianBlur.Name = "GaussianBlur Pipeline";
      gaussianBlur.DepthSpec.DepthEnable = false;
      gaussianBlur.SetDescriptions = {
        {
          SetDescription{0, 0, 1, vDT::eStorageImage, vSS::eCompute},
          SetDescription{1, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
        }
      };
      gaussianBlur.Shader = gaussianBlurShader.get();
      gaussianBlur.PushConstantRanges.emplace_back(vk::ShaderStageFlagBits::eCompute, 0, 4);
      s_Pipelines.GaussianBlurPipeline.CreateComputePipelineAsync(gaussianBlur).wait();
    }
    {
      PipelineDescription bloomDesc;
      bloomDesc.Name = "Bloom Pipeline";
      bloomDesc.ColorAttachmentCount = 1;
      bloomDesc.DepthSpec.DepthEnable = false;
      bloomDesc.PushConstantRanges.emplace_back(vk::ShaderStageFlagBits::eCompute, 0, 40);
      bloomDesc.SetDescriptions = {
        {
          SetDescription{0, 0, 3, vDT::eCombinedImageSampler, vSS::eCompute},
          SetDescription{1, 0, 9, vDT::eStorageImage, vSS::eCompute},
          SetDescription{2, 0, 8, vDT::eStorageImage, vSS::eCompute},
        }
      };
      bloomDesc.Shader = bloomShader.get();
      s_Pipelines.BloomPipeline.CreateComputePipelineAsync(bloomDesc).wait();
    }
    {
      PipelineDescription ssrDesc;
      ssrDesc.Name = "SSR Pipeline";
      ssrDesc.SetDescriptions = {
        {
          SetDescription{0, 0, 1, vDT::eStorageImage, vSS::eCompute},
          SetDescription{1, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
          SetDescription{2, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
          SetDescription{3, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
          SetDescription{4, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
          SetDescription{5, 0, 1, vDT::eUniformBuffer, vSS::eCompute, nullptr, &s_RendererData.VSBuffer.GetDescriptor()},
          SetDescription{6, 0, 1, vDT::eUniformBuffer, vSS::eCompute, nullptr, &s_RendererData.SSRBuffer.GetDescriptor()},
        }
      };
      ssrDesc.Shader = ssrShader.get();
      s_Pipelines.SSRPipeline.CreateComputePipelineAsync(ssrDesc).wait();
    }
    {
      PipelineDescription atmDesc;
      atmDesc.Name = "Atmosphere Pipeline";
      atmDesc.SetDescriptions = {
        {
          SetDescription{0, 0, 1, vDT::eStorageImage, vSS::eCompute},
          SetDescription{1, 0, 1, vDT::eUniformBuffer, vSS::eCompute, nullptr, &s_RendererData.AtmosphereBuffer.GetDescriptor()},
        }
      };
      atmDesc.Shader = atmosphereShader.get();
      s_Pipelines.AtmospherePipeline.CreateComputePipelineAsync(atmDesc).wait();
    }
    {
      PipelineDescription depthOfField;
      depthOfField.Name = "DOF Pipeline";
      depthOfField.SetDescriptions = {
        {
          SetDescription{0, 0, 1, vDT::eStorageImage, vSS::eCompute},
          SetDescription{1, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
          SetDescription{2, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
        }
      };
      depthOfField.Shader = depthOfFieldShader.get();
      s_Pipelines.DepthOfFieldPipeline.CreateComputePipelineAsync(depthOfField).wait();
    }
    {
      PipelineDescription composite;
      composite.DepthSpec.DepthEnable = false;
      composite.SetDescriptions = {
        {
          SetDescription{0, 0, 1, vDT::eStorageImage, vSS::eCompute},
          SetDescription{1, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
          SetDescription{2, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
          SetDescription{3, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
          SetDescription{4, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
          SetDescription{5, 0, 1, vDT::eUniformBuffer, vSS::eCompute, nullptr, &s_RendererData.PostProcessBuffer.GetDescriptor()},
        }
      };
      composite.Shader = compositeShader.get();
      s_Pipelines.CompositePipeline.CreateComputePipelineAsync(composite).wait();
    }
    {
      PipelineDescription ppPass;
      ppPass.SetDescriptions = {
        {
          SetDescription{0, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment},
          SetDescription{1, 0, 1, vDT::eUniformBuffer, vSS::eFragment, nullptr, &s_RendererData.PostProcessBuffer.GetDescriptor()},
        }
      };
      ppPass.Shader = postProcessShader.get();
      ppPass.RasterizerDesc.CullMode = vk::CullModeFlagBits::eNone;
      ppPass.VertexInputState = VertexInputDescription(VertexLayout({
        VertexComponent::POSITION, VertexComponent::NORMAL, VertexComponent::UV
      }));
      ppPass.DepthSpec.DepthEnable = false;
      s_Pipelines.PostProcessPipeline.CreateGraphicsPipelineAsync(ppPass).wait();
    }
    {
      PipelineDescription quadDescription;
      quadDescription.SetDescriptions = {
        {
          SetDescription{7, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment},
        }
      };
      quadDescription.RenderPass = SwapChain.m_RenderPass;
      quadDescription.VertexInputState.attributeDescriptions.clear();
      quadDescription.VertexInputState.bindingDescriptions.clear();
      quadDescription.Shader = quadShader.get();
      quadDescription.RasterizerDesc.CullMode = vk::CullModeFlagBits::eNone;
      s_Pipelines.QuadPipeline.CreateGraphicsPipelineAsync(quadDescription).wait();
    }

    PipelineDescription computePipelineDesc;
    computePipelineDesc.SetDescriptions = {
      {
        SetDescription{0, 0, 1, vDT::eUniformBuffer, vSS::eCompute, nullptr, &s_RendererData.VSBuffer.GetDescriptor()},
        SetDescription{1, 0, 1, vDT::eUniformBuffer, vSS::eCompute, nullptr, &s_RendererData.ParametersBuffer.GetDescriptor()},
        SetDescription{2, 0, 1, vDT::eStorageBuffer, vSS::eCompute, nullptr, &s_RendererData.LightsBuffer.GetDescriptor()},
        SetDescription{3, 0, 1, vDT::eStorageBuffer, vSS::eCompute, nullptr, &s_RendererData.FrustumBuffer.GetDescriptor()},
        SetDescription{4, 0, 1, vDT::eStorageBuffer, vSS::eCompute, nullptr, &s_RendererData.LighIndexBuffer.GetDescriptor()},
        SetDescription{5, 0, 1, vDT::eStorageBuffer, vSS::eCompute, nullptr, &s_RendererData.LighGridBuffer.GetDescriptor()},
        SetDescription{6, 0, 1, vDT::eCombinedImageSampler, vSS::eCompute},
      }
    };
    computePipelineDesc.Shader = frustumGridShader.get();
    s_Pipelines.FrustumGridPipeline.CreateComputePipelineAsync(computePipelineDesc).wait();

    computePipelineDesc.Shader = lightListShader.get();
    s_Pipelines.LightListPipeline.CreateComputePipelineAsync(computePipelineDesc).wait();

    const std::vector vertexInputBindings = {
      vk::VertexInputBindingDescription{0, sizeof(ImDrawVert), vk::VertexInputRate::eVertex},
    };
    const std::vector vertexInputAttributes = {
      vk::VertexInputAttributeDescription{0, 0, vk::Format::eR32G32Sfloat, (uint32_t)offsetof(ImDrawVert, pos)},
      vk::VertexInputAttributeDescription{1, 0, vk::Format::eR32G32Sfloat, (uint32_t)offsetof(ImDrawVert, uv)},
      vk::VertexInputAttributeDescription{2, 0, vk::Format::eR8G8B8A8Unorm, (uint32_t)offsetof(ImDrawVert, col)},
    };
    PipelineDescription uiPipelineDecs;
    uiPipelineDecs.Name = "UI Pipeline";
    uiPipelineDecs.Shader = uiShader.get();
    uiPipelineDecs.SetDescriptions = {
      {
        SetDescription{0, 0, 1, vDT::eCombinedImageSampler, vSS::eFragment},
      }
    };
    uiPipelineDecs.PushConstantRanges = {vk::PushConstantRange{vk::ShaderStageFlagBits::eVertex, 0, sizeof(float) * 4}};
    uiPipelineDecs.RenderPass = SwapChain.m_RenderPass;
    uiPipelineDecs.VertexInputState.attributeDescriptions = vertexInputAttributes;
    uiPipelineDecs.VertexInputState.bindingDescriptions = vertexInputBindings;
    uiPipelineDecs.RasterizerDesc.CullMode = vk::CullModeFlagBits::eNone;
    uiPipelineDecs.BlendStateDesc.RenderTargets[0].BlendEnable = true;
    uiPipelineDecs.BlendStateDesc.RenderTargets[0].SrcBlend = vk::BlendFactor::eSrcAlpha;
    uiPipelineDecs.BlendStateDesc.RenderTargets[0].DestBlend = vk::BlendFactor::eOneMinusSrcAlpha;
    uiPipelineDecs.BlendStateDesc.RenderTargets[0].BlendOp = vk::BlendOp::eAdd;
    uiPipelineDecs.BlendStateDesc.RenderTargets[0].SrcBlendAlpha = vk::BlendFactor::eOne;
    uiPipelineDecs.BlendStateDesc.RenderTargets[0].DestBlendAlpha = vk::BlendFactor::eOneMinusSrcAlpha;
    uiPipelineDecs.BlendStateDesc.RenderTargets[0].BlendOpAlpha = vk::BlendOp::eAdd;
    uiPipelineDecs.BlendStateDesc.RenderTargets[0].WriteMask = {
      vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB |
      vk::ColorComponentFlagBits::eA
    };
    uiPipelineDecs.DepthSpec.DepthEnable = false;
    uiPipelineDecs.DepthSpec.DepthWriteEnable = false;
    uiPipelineDecs.DepthSpec.CompareOp = vk::CompareOp::eNever;
    uiPipelineDecs.DepthSpec.FrontFace.StencilFunc = vk::CompareOp::eNever;
    uiPipelineDecs.DepthSpec.BackFace.StencilFunc = vk::CompareOp::eNever;
    uiPipelineDecs.DepthSpec.BoundTest = false;
    uiPipelineDecs.DepthSpec.MinDepthBound = 0;
    uiPipelineDecs.DepthSpec.MaxDepthBound = 0;

    s_Pipelines.UIPipeline.CreateGraphicsPipelineAsync(uiPipelineDecs).wait();
  }

  void VulkanRenderer::CreateFramebuffers() {
    ZoneScoped;
    VulkanImageDescription colorImageDesc{};
    colorImageDesc.Format = SwapChain.m_ImageFormat;
    colorImageDesc.UsageFlags = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
    colorImageDesc.ImageTiling = vk::ImageTiling::eOptimal;
    colorImageDesc.Width = SwapChain.m_Extent.width;
    colorImageDesc.Height = SwapChain.m_Extent.height;
    colorImageDesc.CreateView = true;
    colorImageDesc.CreateSampler = true;
    colorImageDesc.CreateDescriptorSet = true;
    colorImageDesc.AspectFlag = vk::ImageAspectFlagBits::eColor;
    colorImageDesc.FinalImageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    colorImageDesc.SamplerAddressMode = vk::SamplerAddressMode::eClampToEdge;
    {
      VulkanImageDescription depthImageDesc{};
      depthImageDesc.Format = vk::Format::eD32Sfloat;
      depthImageDesc.Height = SwapChain.m_Extent.height;
      depthImageDesc.Width = SwapChain.m_Extent.width;
      depthImageDesc.UsageFlags = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled;
      depthImageDesc.ImageTiling = vk::ImageTiling::eOptimal;
      depthImageDesc.CreateSampler = true;
      depthImageDesc.CreateDescriptorSet = true;
      depthImageDesc.DescriptorSetLayout = s_RendererData.ImageDescriptorSetLayout;
      depthImageDesc.CreateView = true;
      depthImageDesc.AspectFlag = vk::ImageAspectFlagBits::eDepth;
      depthImageDesc.FinalImageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
      depthImageDesc.TransitionLayoutAtCreate = true;
      FramebufferDescription framebufferDescription;
      framebufferDescription.DebugName = "Depth Pass";
      framebufferDescription.RenderPass = s_Pipelines.DepthPrePassPipeline.GetRenderPass().Get();
      framebufferDescription.Width = SwapChain.m_Extent.width;
      framebufferDescription.Height = SwapChain.m_Extent.height;
      framebufferDescription.Extent = &Window::GetWindowExtent();
      framebufferDescription.ImageDescription = {depthImageDesc, colorImageDesc};
      framebufferDescription.OnResize = [] {
        s_DepthDescriptorSet.WriteDescriptorSets[0].pBufferInfo = &s_RendererData.VSBuffer.GetDescriptor();
        s_ComputeDescriptorSet.WriteDescriptorSets[6].pImageInfo = &s_FrameBuffers.DepthNormalPassFB.GetImage()[0].GetDescImageInfo();
        UpdateComputeDescriptorSets();
      };
      s_FrameBuffers.DepthNormalPassFB.CreateFramebuffer(framebufferDescription);

      //Direct shadow depth pass
      depthImageDesc.Format = vk::Format::eD32Sfloat;
      depthImageDesc.Width = RendererConfig::Get()->DirectShadowsConfig.Size;
      depthImageDesc.Height = RendererConfig::Get()->DirectShadowsConfig.Size;
      framebufferDescription.Extent = nullptr;
      depthImageDesc.Type = ImageType::TYPE_2DARRAY;
      depthImageDesc.ImageArrayLayerCount = SHADOW_MAP_CASCADE_COUNT;
      depthImageDesc.ViewArrayLayerCount = SHADOW_MAP_CASCADE_COUNT;
      depthImageDesc.FinalImageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
      depthImageDesc.TransitionLayoutAtCreate = true;
      depthImageDesc.BaseArrayLayerIndex = 0;
      s_Resources.DirectShadowsDepthArray.Create(depthImageDesc);

      framebufferDescription.Width = RendererConfig::Get()->DirectShadowsConfig.Size;
      framebufferDescription.Height = RendererConfig::Get()->DirectShadowsConfig.Size;
      framebufferDescription.DebugName = "Direct Shadow Depth Pass";
      framebufferDescription.RenderPass = s_Pipelines.DirectShadowDepthPipeline.GetRenderPass().Get();
      framebufferDescription.OnResize = [] {
        s_ShadowDepthDescriptorSet.WriteDescriptorSets[0].pBufferInfo = &s_RendererData.DirectShadowBuffer.GetDescriptor();
        s_ShadowDepthDescriptorSet.Update();
      };
      s_FrameBuffers.DirectionalCascadesFB.resize(SHADOW_MAP_CASCADE_COUNT);
      int baseArrayLayer = 0;
      depthImageDesc.ViewArrayLayerCount = 1;
      for (auto& fb : s_FrameBuffers.DirectionalCascadesFB) {
        depthImageDesc.BaseArrayLayerIndex = baseArrayLayer;
        framebufferDescription.ImageDescription = {depthImageDesc};
        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.viewType = vk::ImageViewType::e2DArray;
        viewInfo.format = vk::Format::eD32Sfloat;
        viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = baseArrayLayer;
        viewInfo.subresourceRange.layerCount = 1;
        viewInfo.image = s_Resources.DirectShadowsDepthArray.GetImage();
        const auto& LogicalDevice = VulkanContext::Context.Device;
        vk::ImageView view;
        VulkanUtils::CheckResult(LogicalDevice.createImageView(&viewInfo, nullptr, &view));
        fb.CreateFramebufferWithImageView(framebufferDescription, view);
        baseArrayLayer++;
      }
    }
    {
      VulkanImageDescription DepthImageDesc;
      DepthImageDesc.Format = vk::Format::eD32Sfloat;
      DepthImageDesc.UsageFlags = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                  vk::ImageUsageFlagBits::eInputAttachment;
      DepthImageDesc.ImageTiling = vk::ImageTiling::eOptimal;
      DepthImageDesc.Width = SwapChain.m_Extent.width;
      DepthImageDesc.Height = SwapChain.m_Extent.height;
      DepthImageDesc.CreateView = true;
      DepthImageDesc.CreateSampler = true;
      DepthImageDesc.CreateDescriptorSet = false;
      DepthImageDesc.AspectFlag = vk::ImageAspectFlagBits::eDepth;
      DepthImageDesc.FinalImageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

      FramebufferDescription framebufferDescription;
      framebufferDescription.DebugName = "PBR Pass";
      framebufferDescription.Width = SwapChain.m_Extent.width;
      framebufferDescription.Height = SwapChain.m_Extent.height;
      framebufferDescription.Extent = &Window::GetWindowExtent();
      framebufferDescription.RenderPass = s_Pipelines.PBRPipeline.GetRenderPass().Get();
      framebufferDescription.ImageDescription = {colorImageDesc, DepthImageDesc};
      framebufferDescription.OnResize = [] {
        s_QuadDescriptorSet.WriteDescriptorSets[0].pImageInfo = &s_FrameBuffers.PBRPassFB.GetImage()[0].GetDescImageInfo();
        s_QuadDescriptorSet.Update();
      };
      s_FrameBuffers.PBRPassFB.CreateFramebuffer(framebufferDescription);
    }
    {
      VulkanImageDescription ssaopassimage;
      ssaopassimage.Height = Window::GetHeight();
      ssaopassimage.Width = Window::GetWidth();
      ssaopassimage.CreateDescriptorSet = true;
      ssaopassimage.UsageFlags = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
      ssaopassimage.Format = vk::Format::eR8Unorm;
      ssaopassimage.FinalImageLayout = vk::ImageLayout::eGeneral;
      ssaopassimage.TransitionLayoutAtCreate = true;
      ssaopassimage.SamplerAddressMode = vk::SamplerAddressMode::eClampToEdge;
      ssaopassimage.SamplerBorderColor = vk::BorderColor::eFloatTransparentBlack;
      s_FrameBuffers.SSAOPassImage.Create(ssaopassimage);

      ImagePool::AddToPool(
        &s_FrameBuffers.SSAOPassImage,
        &Window::GetWindowExtent(),
        [] {
          UpdateSSAODescriptorSets();
          s_FrameBuffers.PostProcessPassFB.GetDescription().OnResize();
        });

      s_FrameBuffers.SSAOBlurPassImage.Create(ssaopassimage);

      ImagePool::AddToPool(
        &s_FrameBuffers.SSAOBlurPassImage,
        &Window::GetWindowExtent(),
        [] {
          UpdateSSAODescriptorSets();
          s_FrameBuffers.PostProcessPassFB.GetDescription().OnResize();
        });
    }
    {
      VulkanImageDescription ssrPassImage;
      ssrPassImage.Height = Window::GetHeight();
      ssrPassImage.Width = Window::GetWidth();
      ssrPassImage.CreateDescriptorSet = true;
      ssrPassImage.UsageFlags = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
      ssrPassImage.Format = SwapChain.m_ImageFormat;
      ssrPassImage.FinalImageLayout = vk::ImageLayout::eGeneral;
      ssrPassImage.TransitionLayoutAtCreate = true;
      ssrPassImage.SamplerAddressMode = vk::SamplerAddressMode::eClampToBorder;
      ssrPassImage.SamplerBorderColor = vk::BorderColor::eFloatTransparentBlack;
      s_FrameBuffers.SSRPassImage.Create(ssrPassImage);

      ImagePool::AddToPool(
        &s_FrameBuffers.SSRPassImage,
        &Window::GetWindowExtent(),
        [] {
          s_SSRDescriptorSet.WriteDescriptorSets[0].pImageInfo = &s_FrameBuffers.SSRPassImage.GetDescImageInfo();
          s_SSRDescriptorSet.WriteDescriptorSets[1].pImageInfo = &s_FrameBuffers.PBRPassFB.GetImage()[0].GetDescImageInfo();
          s_SSRDescriptorSet.WriteDescriptorSets[2].pImageInfo = &s_FrameBuffers.DepthNormalPassFB.GetImage()[0].GetDescImageInfo();
          s_SSRDescriptorSet.WriteDescriptorSets[3].pImageInfo = &s_Resources.CubeMap.GetDescImageInfo();
          s_SSRDescriptorSet.WriteDescriptorSets[4].pImageInfo = &s_FrameBuffers.DepthNormalPassFB.GetImage()[1].GetDescImageInfo();
          s_SSRDescriptorSet.Update();
          s_FrameBuffers.PostProcessPassFB.GetDescription().OnResize();
        });
    }
    {
      VulkanImageDescription atmImage;
      atmImage.Height = 128;
      atmImage.Width = 128;
      atmImage.CreateDescriptorSet = true;
      atmImage.Type = ImageType::TYPE_CUBE;
      atmImage.UsageFlags = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
      atmImage.Format = SwapChain.m_ImageFormat;
      atmImage.FinalImageLayout = vk::ImageLayout::eGeneral;
      atmImage.TransitionLayoutAtCreate = true;
      s_FrameBuffers.AtmosphereImage.Create(atmImage);

      ImagePool::AddToPool(
        &s_FrameBuffers.AtmosphereImage,
        nullptr,
        [] {
          s_AtmosphereDescriptorSet.WriteDescriptorSets[0].pImageInfo = &s_FrameBuffers.AtmosphereImage.GetDescImageInfo();
          s_AtmosphereDescriptorSet.Update();
          s_FrameBuffers.PostProcessPassFB.GetDescription().OnResize();
        });
    }
    {
      VulkanImageDescription dofImage;
      dofImage.Height = Window::GetHeight();
      dofImage.Width = Window::GetWidth();
      dofImage.CreateDescriptorSet = true;
      dofImage.UsageFlags = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
      dofImage.Format = SwapChain.m_ImageFormat;
      dofImage.FinalImageLayout = vk::ImageLayout::eGeneral;
      dofImage.TransitionLayoutAtCreate = true;
      dofImage.SamplerAddressMode = vk::SamplerAddressMode::eClampToBorder;
      dofImage.SamplerBorderColor = vk::BorderColor::eFloatTransparentBlack;
      s_FrameBuffers.DepthOfFieldImage.Create(dofImage);

      ImagePool::AddToPool(
        &s_FrameBuffers.DepthOfFieldImage,
        &Window::GetWindowExtent(),
        [] {
          s_DepthOfFieldDescriptorSet.WriteDescriptorSets[0].pImageInfo = &s_FrameBuffers.DepthOfFieldImage.GetDescImageInfo();
          s_DepthOfFieldDescriptorSet.WriteDescriptorSets[1].pImageInfo = &s_FrameBuffers.PBRPassFB.GetImage()[0].GetDescImageInfo();
          s_DepthOfFieldDescriptorSet.WriteDescriptorSets[2].pImageInfo = &s_FrameBuffers.DepthNormalPassFB.GetImage()[0].GetDescImageInfo();
          s_DepthOfFieldDescriptorSet.Update();
          s_FrameBuffers.PostProcessPassFB.GetDescription().OnResize();
        });
    }
    {
      const auto lodCount = std::max((int32_t)VulkanImage::GetMaxMipmapLevel(Window::GetWidth() / 2, Window::GetHeight() / 2, 1), 2);
      VulkanImageDescription bloompassimage;
      bloompassimage.Width = Window::GetWidth() / 2;
      bloompassimage.Height = Window::GetHeight() / 2;
      bloompassimage.CreateDescriptorSet = true;
      bloompassimage.UsageFlags = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
      bloompassimage.Format = SwapChain.m_ImageFormat;
      bloompassimage.FinalImageLayout = vk::ImageLayout::eGeneral;
      bloompassimage.TransitionLayoutAtCreate = true;
      bloompassimage.SamplerAddressMode = vk::SamplerAddressMode::eClampToEdge;
      bloompassimage.SamplerBorderColor = vk::BorderColor::eFloatTransparentBlack;
      bloompassimage.MinFiltering = vk::Filter::eNearest;
      bloompassimage.MagFiltering = vk::Filter::eLinear;
      bloompassimage.MipLevels = lodCount;
      s_FrameBuffers.BloomDownsampleImage.Create(bloompassimage);
      bloompassimage.MipLevels = lodCount - 1;
      s_FrameBuffers.BloomUpsampleImage.Create(bloompassimage);
      auto updateBloomSet = [] {
        const std::vector samplerImageInfos = {
          s_FrameBuffers.PBRPassFB.GetImage()[0].GetDescImageInfo(),
          s_FrameBuffers.BloomDownsampleImage.GetDescImageInfo(),
          s_FrameBuffers.BloomUpsampleImage.GetDescImageInfo()
        };
        s_BloomDescriptorSet.WriteDescriptorSets[0].pImageInfo = samplerImageInfos.data();
        const auto& downsamplerViews = s_FrameBuffers.BloomDownsampleImage.GetMipDescriptors();
        s_BloomDescriptorSet.WriteDescriptorSets[1].pImageInfo = downsamplerViews.data();
        const auto& upsamplerViews = s_FrameBuffers.BloomUpsampleImage.GetMipDescriptors();
        s_BloomDescriptorSet.WriteDescriptorSets[2].pImageInfo = upsamplerViews.data();
        s_BloomDescriptorSet.Update();

        s_FrameBuffers.PostProcessPassFB.GetDescription().OnResize();
      };
      ImagePool::AddToPool(
        &s_FrameBuffers.BloomUpsampleImage,
        &Window::GetWindowExtent(),
        [updateBloomSet] {
          updateBloomSet();
        },
        2);
      ImagePool::AddToPool(
        &s_FrameBuffers.BloomDownsampleImage,
        &Window::GetWindowExtent(),
        [updateBloomSet] {
          updateBloomSet();
        },
        2);
    }
    {
      VulkanImageDescription composite;
      composite.Height = Window::GetHeight();
      composite.Width = Window::GetWidth();
      composite.CreateDescriptorSet = true;
      composite.UsageFlags = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
      composite.Format = SwapChain.m_ImageFormat;
      composite.FinalImageLayout = vk::ImageLayout::eGeneral;
      composite.TransitionLayoutAtCreate = true;
      composite.SamplerAddressMode = vk::SamplerAddressMode::eClampToBorder;
      composite.SamplerBorderColor = vk::BorderColor::eFloatTransparentBlack;
      s_FrameBuffers.CompositePassImage.Create(composite);

      ImagePool::AddToPool(
        &s_FrameBuffers.CompositePassImage,
        &Window::GetWindowExtent(),
        [] {
          s_CompositeDescriptorSet.WriteDescriptorSets[0].pImageInfo = &s_FrameBuffers.CompositePassImage.GetDescImageInfo();
          s_CompositeDescriptorSet.WriteDescriptorSets[1].pImageInfo = &s_FrameBuffers.DepthOfFieldImage.GetDescImageInfo();
          s_CompositeDescriptorSet.WriteDescriptorSets[2].pImageInfo = &s_FrameBuffers.SSAOBlurPassImage.GetDescImageInfo();
          s_CompositeDescriptorSet.WriteDescriptorSets[3].pImageInfo = &s_FrameBuffers.BloomUpsampleImage.GetDescImageInfo();
          s_CompositeDescriptorSet.WriteDescriptorSets[4].pImageInfo = &s_FrameBuffers.SSRPassImage.GetDescImageInfo();
          s_CompositeDescriptorSet.WriteDescriptorSets[5].pImageInfo = &s_FrameBuffers.PostProcessPassFB.GetImage()[0].GetDescImageInfo();
          s_CompositeDescriptorSet.Update();
          s_FrameBuffers.PostProcessPassFB.GetDescription().OnResize();
        });
    }
    {
      FramebufferDescription postProcess;
      postProcess.DebugName = "Post Process Pass";
      postProcess.Width = Window::GetWidth();
      postProcess.Height = Window::GetHeight();
      postProcess.Extent = &Window::GetWindowExtent();
      postProcess.RenderPass = s_Pipelines.PostProcessPipeline.GetRenderPass().Get();
      colorImageDesc.Format = SwapChain.m_ImageFormat;
      postProcess.ImageDescription = {colorImageDesc};
      postProcess.OnResize = [] {
        s_PostProcessDescriptorSet.WriteDescriptorSets[0].pImageInfo = &s_FrameBuffers.CompositePassImage.GetDescImageInfo();
        s_PostProcessDescriptorSet.Update();
      };
      s_FrameBuffers.PostProcessPassFB.CreateFramebuffer(postProcess);
    }
  }

  void VulkanRenderer::ResizeBuffers() {
    WaitDeviceIdle();
    WaitGraphicsQueueIdle();
    SwapChain.RecreateSwapChain();
    FrameBufferPool::ResizeBuffers();
    ImagePool::ResizeImages();
    SwapChain.Resizing = false;
  }

  void VulkanRenderer::UpdateSkyboxDescriptorSets() {
    const auto& LogicalDevice = VulkanContext::Context.Device;
    LogicalDevice.updateDescriptorSets(
      {
        {s_SkyboxDescriptorSet.Get(), 0, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &s_RendererData.SkyboxBuffer.GetDescriptor()},
        {s_SkyboxDescriptorSet.Get(), 1, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &s_RendererData.PostProcessBuffer.GetDescriptor()},
        {s_SkyboxDescriptorSet.Get(), 6, 0, 1, vk::DescriptorType::eCombinedImageSampler, &s_Resources.CubeMap.GetDescImageInfo()},
      },
      nullptr);
  }

  void VulkanRenderer::UpdateComputeDescriptorSets() {
    s_ComputeDescriptorSet.WriteDescriptorSets[6].pImageInfo = &s_FrameBuffers.DepthNormalPassFB.GetImage()[0].GetDescImageInfo();
    s_ComputeDescriptorSet.Update();
  }

  void VulkanRenderer::UpdateSSAODescriptorSets() {
    s_SSAODescriptorSet.WriteDescriptorSets[1].pImageInfo = &s_FrameBuffers.DepthNormalPassFB.GetImage()[0].GetDescImageInfo();
    s_SSAODescriptorSet.WriteDescriptorSets[2].pImageInfo = &s_FrameBuffers.SSAOPassImage.GetDescImageInfo();
    s_SSAODescriptorSet.WriteDescriptorSets[3].pImageInfo = &s_FrameBuffers.DepthNormalPassFB.GetImage()[1].GetDescImageInfo();
    s_SSAODescriptorSet.Update();

    s_SSAOBlurDescriptorSet.WriteDescriptorSets[0].pImageInfo = &s_FrameBuffers.SSAOBlurPassImage.GetDescImageInfo();
    s_SSAOBlurDescriptorSet.WriteDescriptorSets[1].pImageInfo = &s_FrameBuffers.SSAOPassImage.GetDescImageInfo();
    s_SSAOBlurDescriptorSet.Update();
  }

  void VulkanRenderer::InitRenderGraph() {
    auto& renderGraph = s_RendererContext.RenderGraph;

    SwapchainPass swapchain{&s_QuadDescriptorSet};
    renderGraph.SetSwapchain(swapchain);

    RenderGraphPass depthPrePass(
      "Depth Pre Pass",
      {&s_RendererContext.DepthPassCommandBuffer},
      &s_Pipelines.DepthPrePassPipeline,
      {&s_FrameBuffers.DepthNormalPassFB},
      [](VulkanCommandBuffer& commandBuffer, int32_t) {
        ZoneScopedN("DepthPrePass");
        OX_TRACE_GPU(commandBuffer.Get(), "Depth Pre Pass")
        commandBuffer.SetViwportWindow().SetScissorWindow();
        for (const auto& mesh : s_MeshDrawList) {
          if (!mesh.MeshGeometry)
            continue;

          RenderMesh(mesh,
            s_RendererContext.DepthPassCommandBuffer.Get(),
            s_Pipelines.DepthPrePassPipeline,
            [&](const Mesh::Primitive* part) {
              const auto& material = mesh.Materials[part->materialIndex];
              if (!material->IsOpaque())
                return false;
              const auto layout = s_Pipelines.DepthPrePassPipeline.GetPipelineLayout();
              commandBuffer.PushConstants(layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4), &mesh.Transform);
              commandBuffer.PushConstants(layout, vk::ShaderStageFlagBits::eFragment, sizeof(glm::mat4), sizeof Material::Parameters, &material->Parameters);
              s_Pipelines.DepthPrePassPipeline.BindDescriptorSets(commandBuffer.Get(), {Material::s_DescriptorSet.Get(), material->MaterialDescriptorSet.Get()}, 0, 2);
              return true;
            });
        }
      },
      {vk::ClearDepthStencilValue{1.0f, 0}, vk::ClearColorValue(std::array{0.0f, 0.0f, 0.0f, 1.0f})},
      &VulkanContext::VulkanQueue.GraphicsQueue);
    renderGraph.AddRenderPass(depthPrePass);

    std::array<vk::ClearValue, 2> clearValues;
    clearValues[0].color = vk::ClearColorValue(std::array{0.0f, 0.0f, 0.0f, 1.0f});
    clearValues[1].depthStencil = vk::ClearDepthStencilValue{1.0f, 0};

    RenderGraphPass directShadowDepthPass(
      "Direct Shadow Depth Pass",
      {&s_RendererContext.DirectShadowCommandBuffer},
      &s_Pipelines.DirectShadowDepthPipeline,
      {
        {
          &s_FrameBuffers.DirectionalCascadesFB[0], &s_FrameBuffers.DirectionalCascadesFB[1],
          &s_FrameBuffers.DirectionalCascadesFB[2], &s_FrameBuffers.DirectionalCascadesFB[3]
        }
      },
      [](VulkanCommandBuffer& commandBuffer, int32_t framebufferIndex) {
        ZoneScopedN("DirectShadowDepthPass");
        OX_TRACE_GPU(commandBuffer.Get(), "Direct Shadow Depth Pass")
        commandBuffer.SetViewport(vk::Viewport{
          0, 0, (float)RendererConfig::Get()->DirectShadowsConfig.Size,
          (float)RendererConfig::Get()->DirectShadowsConfig.Size, 0, 1
        }).SetScissor(vk::Rect2D{
          {}, {RendererConfig::Get()->DirectShadowsConfig.Size, RendererConfig::Get()->DirectShadowsConfig.Size,}
        });
        for (const auto& e : s_SceneLights) {
          const auto& lightComponent = e.GetComponent<LightComponent>();
          if (lightComponent.Type != LightComponent::LightType::Directional)
            continue;

          glm::mat4 transform = e.GetWorldTransform();
          UpdateCascades(transform, s_RendererContext.CurrentCamera, s_RendererData.UBO_DirectShadow);
          s_RendererData.DirectShadowBuffer.Copy(&s_RendererData.UBO_DirectShadow, sizeof s_RendererData.UBO_DirectShadow);

          for (const auto& mesh : s_MeshDrawList) {
            if (!mesh.MeshGeometry)
              continue;
            RenderMesh(mesh,
              commandBuffer.Get(),
              s_Pipelines.DirectShadowDepthPipeline,
              [&](const Mesh::Primitive*) {
                struct PushConst {
                  glm::mat4 modelMatrix{};
                  uint32_t cascadeIndex = 0;
                } pushConst;
                pushConst.modelMatrix = mesh.Transform;
                pushConst.cascadeIndex = framebufferIndex;
                const auto& layout = s_Pipelines.DirectShadowDepthPipeline.GetPipelineLayout();
                commandBuffer.PushConstants(layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(PushConst), &pushConst);
                s_Pipelines.DirectShadowDepthPipeline.BindDescriptorSets(commandBuffer.Get(), {s_ShadowDepthDescriptorSet.Get()});
                return true;
              });
          }
        }
      },
      {vk::ClearDepthStencilValue{1.0f, 0}, vk::ClearColorValue(std::array{0.0f, 0.0f, 0.0f, 1.0f})},
      &VulkanContext::VulkanQueue.GraphicsQueue);
    directShadowDepthPass.SetRenderArea(vk::Rect2D{
      {}, {RendererConfig::Get()->DirectShadowsConfig.Size, RendererConfig::Get()->DirectShadowsConfig.Size},
    }).AddToGraph(renderGraph);

    RenderGraphPass ssaoPass(
      "SSAO Pass",
      {&s_RendererContext.SSAOCommandBuffer},
      &s_Pipelines.SSAOPassPipeline,
      {},
      [](const VulkanCommandBuffer& commandBuffer, int32_t) {
        ZoneScopedN("SSAOPass");
        OX_TRACE_GPU(commandBuffer.Get(), "SSAO Pass")
        s_Pipelines.SSAOPassPipeline.BindPipeline(commandBuffer.Get());
        s_Pipelines.SSAOPassPipeline.BindDescriptorSets(commandBuffer.Get(), {s_SSAODescriptorSet.Get()});
        commandBuffer.Dispatch((Window::GetWidth() + 8 - 1) / 8, (Window::GetHeight() + 8 - 1) / 8, 1);
      },
      {clearValues},
      &VulkanContext::VulkanQueue.GraphicsQueue);
    ssaoPass
     .RunWithCondition(RendererConfig::Get()->SSAOConfig.Enabled)
     .AddInnerPass(RenderGraphPass(
        "SSAO Blur Pass",
        {},
        &s_Pipelines.GaussianBlurPipeline,
        {},
        [](VulkanCommandBuffer& commandBuffer, int32_t) {
          ZoneScopedN("SSAO Blur Pass");
          OX_TRACE_GPU(commandBuffer.Get(), "SSAO Blur Pass")
          vk::ImageMemoryBarrier imageMemoryBarrier{};
          imageMemoryBarrier.image = s_FrameBuffers.SSAOPassImage.GetImage();
          imageMemoryBarrier.oldLayout = vk::ImageLayout::eGeneral;
          imageMemoryBarrier.newLayout = vk::ImageLayout::eGeneral;
          imageMemoryBarrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
          imageMemoryBarrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
          imageMemoryBarrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
          imageMemoryBarrier.subresourceRange.levelCount = 1;
          imageMemoryBarrier.subresourceRange.layerCount = 1;
          commandBuffer.Get().pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eComputeShader,
            vk::DependencyFlagBits::eByRegion,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &imageMemoryBarrier);
          const auto& layout = s_Pipelines.GaussianBlurPipeline.GetPipelineLayout();
          s_Pipelines.GaussianBlurPipeline.BindPipeline(commandBuffer.Get());
          struct PushConst {
            GLSL_BOOL Horizontal = false;
          } pushConst;
          s_Pipelines.GaussianBlurPipeline.BindDescriptorSets(commandBuffer.Get(), {s_SSAOBlurDescriptorSet.Get()});
          commandBuffer.Dispatch((Window::GetWidth() + 8 - 1) / 8, (Window::GetHeight() + 8 - 1) / 8, 1);

          pushConst.Horizontal = true;
          commandBuffer.PushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, 4, &pushConst);
          s_Pipelines.GaussianBlurPipeline.BindDescriptorSets(commandBuffer.Get(), {s_SSAOBlurDescriptorSet.Get()});
          commandBuffer.Dispatch((Window::GetWidth() + 8 - 1) / 8, (Window::GetHeight() + 8 - 1) / 8, 1);
        }
      )).AddToGraphCompute(renderGraph);

    RenderGraphPass pbrPass(
      "PBR Pass",
      {&s_RendererContext.PBRPassCommandBuffer},
      &s_Pipelines.SkyboxPipeline,
      {&s_FrameBuffers.PBRPassFB},
      [](VulkanCommandBuffer& commandBuffer, int32_t) {
        ZoneScopedN("PBRPass");
        OX_TRACE_GPU(commandBuffer.Get(), "PBR Pass")
        commandBuffer.SetViwportWindow().SetScissorWindow();

        //Skybox pass
        s_Pipelines.SkyboxPipeline.BindPipeline(commandBuffer.Get());
        s_Pipelines.SkyboxPipeline.BindDescriptorSets(commandBuffer.Get(), {s_SkyboxDescriptorSet.Get()});
        const auto& skyboxLayout = s_Pipelines.SkyboxPipeline.GetPipelineLayout();
        commandBuffer.PushConstants(skyboxLayout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4), &s_RendererContext.CurrentCamera->SkyboxView);
        s_SkyboxCube.Draw(commandBuffer.Get());

        //PBR pipeline
        for (const auto& mesh : s_MeshDrawList) {
          if (!mesh.MeshGeometry)
            continue;

          RenderMesh(mesh,
            commandBuffer.Get(),
            s_Pipelines.PBRPipeline,
            [&](const Mesh::Primitive* part) {
              const auto& material = mesh.Materials[part->materialIndex];
              const auto& layout = s_Pipelines.PBRPipeline.GetPipelineLayout();
              commandBuffer.PushConstants(layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4), &mesh.Transform);
              commandBuffer.PushConstants(layout, vk::ShaderStageFlagBits::eFragment, sizeof(glm::mat4), sizeof Material::Parameters, &material->Parameters);

              s_Pipelines.PBRPipeline.BindDescriptorSets(commandBuffer.Get(), {Material::s_DescriptorSet.Get(), material->MaterialDescriptorSet.Get()}, 0, 2);
              return true;
            });
        }
        s_ForceUpdateMaterials = false;
        s_MeshDrawList.clear();
      },
      {clearValues},
      &VulkanContext::VulkanQueue.GraphicsQueue);
    pbrPass.AddToGraph(renderGraph);

    RenderGraphPass ssrPass(
      "SSR Pass",
      {&s_RendererContext.SSRCommandBuffer},
      &s_Pipelines.SSRPipeline,
      {},
      [](const VulkanCommandBuffer& commandBuffer, int32_t) {
        ZoneScopedN("SSR Pass");
        OX_TRACE_GPU(commandBuffer.Get(), "SSR Pass")
        s_Pipelines.SSRPipeline.BindPipeline(commandBuffer.Get());
        s_Pipelines.SSRPipeline.BindDescriptorSets(commandBuffer.Get(), {s_SSRDescriptorSet.Get()});
        commandBuffer.Dispatch((Window::GetWidth() + 8 - 1) / 8, (Window::GetHeight() + 8 - 1) / 8, 1);
      },
      clearValues,
      &VulkanContext::VulkanQueue.GraphicsQueue);
    ssrPass.RunWithCondition(s_RendererConfig.SSRConfig.Enabled)
           .AddToGraphCompute(renderGraph);

    RenderGraphPass bloomPass(
      "Bloom Pass",
      {&s_RendererContext.BloomPassCommandBuffer},
      &s_Pipelines.BloomPipeline,
      {},
      [](VulkanCommandBuffer& commandBuffer, int32_t) {
        ZoneScopedN("BloomPass");
        OX_TRACE_GPU(commandBuffer.Get(), "Bloom Pass")
        struct PushConst {
          Vec4 Params = {
            RendererConfig::Get()->BloomConfig.Threshold,
            RendererConfig::Get()->BloomConfig.Clamp,
            {}, {}
          };
          IVec2 Stage{};
        } pushConst;
        enum STAGES {
          PREFILTER_STAGE  = 0,
          DOWNSAMPLE_STAGE = 1,
          UPSAMPLE_STAGE   = 2,
        };
        const int32_t lodCount = (int32_t)s_FrameBuffers.BloomDownsampleImage.GetDesc().MipLevels - 3;

        const auto& layout = s_Pipelines.BloomPipeline.GetPipelineLayout();

        vk::ImageMemoryBarrier imageMemoryBarrier{};
        imageMemoryBarrier.image = s_FrameBuffers.BloomDownsampleImage.GetImage();
        imageMemoryBarrier.oldLayout = vk::ImageLayout::eGeneral;
        imageMemoryBarrier.newLayout = vk::ImageLayout::eGeneral;
        imageMemoryBarrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        imageMemoryBarrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        imageMemoryBarrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        imageMemoryBarrier.subresourceRange.levelCount = lodCount;
        imageMemoryBarrier.subresourceRange.layerCount = 1;

        s_Pipelines.BloomPipeline.BindPipeline(commandBuffer.Get());
        pushConst.Stage.x = PREFILTER_STAGE;
        commandBuffer.PushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof pushConst, &pushConst);

        s_Pipelines.BloomPipeline.BindDescriptorSets(commandBuffer.Get(), {s_BloomDescriptorSet.Get()});
        IVec3 size = VulkanImage::GetMipMapLevelSize(s_FrameBuffers.BloomDownsampleImage.GetWidth(), s_FrameBuffers.BloomDownsampleImage.GetHeight(), 1, 0);
        commandBuffer.Dispatch((size.x + 8 - 1) / 8, (size.y + 8 - 1) / 8, 1);
        commandBuffer.Get().pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
          vk::PipelineStageFlagBits::eComputeShader,
          vk::DependencyFlagBits::eByRegion,
          0,
          nullptr,
          0,
          nullptr,
          1,
          &imageMemoryBarrier);

        //Downsample
        pushConst.Stage.x = DOWNSAMPLE_STAGE;
        for (int32_t i = 1; i < lodCount; i++) {
          size = VulkanImage::GetMipMapLevelSize(s_FrameBuffers.BloomDownsampleImage.GetWidth(), s_FrameBuffers.BloomDownsampleImage.GetHeight(), 1, i);

          //Set lod in shader
          pushConst.Stage.y = i - 1;

          s_Pipelines.BloomPipeline.BindDescriptorSets(commandBuffer.Get(), {s_BloomDescriptorSet.Get()});
          commandBuffer.PushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof pushConst, &pushConst);
          commandBuffer.Dispatch((size.x + 8 - 1) / 8, (size.y + 8 - 1) / 8, 1);
          commandBuffer.Get().pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eComputeShader,
            vk::DependencyFlagBits::eByRegion,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &imageMemoryBarrier);
        }

        //Upsample
        pushConst.Stage.x = UPSAMPLE_STAGE;
        commandBuffer.PushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof pushConst, &pushConst);

        size = VulkanImage::GetMipMapLevelSize(s_FrameBuffers.BloomUpsampleImage.GetWidth(), s_FrameBuffers.BloomUpsampleImage.GetHeight(), 1, lodCount - 1);
        pushConst.Stage.y = lodCount - 1;
        commandBuffer.PushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof pushConst, &pushConst);

        imageMemoryBarrier.subresourceRange.levelCount = s_FrameBuffers.BloomUpsampleImage.GetDesc().MipLevels;
        imageMemoryBarrier.image = s_FrameBuffers.BloomUpsampleImage.GetImage();
        commandBuffer.Dispatch((size.x + 8 - 1) / 8, (size.y + 8 - 1) / 8, 1);
        commandBuffer.Get().pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader, {}, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier);

        for (int32_t i = lodCount - 1; i >= 0; i--) {
          size = VulkanImage::GetMipMapLevelSize(s_FrameBuffers.BloomUpsampleImage.GetWidth(), s_FrameBuffers.BloomUpsampleImage.GetHeight(), 1, i);

          //Set lod in shader
          pushConst.Stage.y = i;

          commandBuffer.Get().pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader, vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier);
          commandBuffer.PushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof pushConst, &pushConst);
          commandBuffer.Dispatch((size.x + 8 - 1) / 8, (size.y + 8 - 1) / 8, 1);
        }
      },
      clearValues,
      &VulkanContext::VulkanQueue.GraphicsQueue);
    bloomPass.RunWithCondition(RendererConfig::Get()->BloomConfig.Enabled)
             .AddToGraphCompute(renderGraph);

    RenderGraphPass dofPass(
      "DepthOfField Pass",
      {&s_RendererContext.DepthOfFieldCommandBuffer},
      &s_Pipelines.DepthOfFieldPipeline,
      {},
      [](const VulkanCommandBuffer& commandBuffer, int32_t) {
        ZoneScopedN("DepthOfField Pass");
        OX_TRACE_GPU(commandBuffer.Get(), "DepthOfField Pass");
        s_Pipelines.DepthOfFieldPipeline.BindPipeline(commandBuffer.Get());
        s_Pipelines.DepthOfFieldPipeline.BindDescriptorSets(commandBuffer.Get(), {s_DepthOfFieldDescriptorSet.Get()});
        commandBuffer.Dispatch((Window::GetWidth() + 8 - 1) / 8, (Window::GetHeight() + 8 - 1) / 8, 6);
      },
      clearValues,
      &VulkanContext::VulkanQueue.GraphicsQueue
    );
    dofPass.AddToGraphCompute(renderGraph);

    RenderGraphPass atmospherePass(
      "Atmosphere Pass",
      {&s_RendererContext.AtmosphereCommandBuffer},
      &s_Pipelines.AtmospherePipeline,
      {},
      [](const VulkanCommandBuffer& commandBuffer, int32_t) {
        ZoneScopedN("Atmosphere Pass");
        OX_TRACE_GPU(commandBuffer.Get(), "Atmosphere Pass")
        vk::ImageSubresourceRange subresourceRange;
        subresourceRange.aspectMask = s_FrameBuffers.AtmosphereImage.GetDesc().AspectFlag;
        subresourceRange.levelCount = 1;
        subresourceRange.layerCount = 6;
        const vk::ImageMemoryBarrier imageMemoryBarrier = {
          {},
          {},
          vk::ImageLayout::eUndefined,
          vk::ImageLayout::eGeneral,
          0,
          0,
          s_FrameBuffers.AtmosphereImage.GetImage(),
          subresourceRange
        };
        commandBuffer.Get().pipelineBarrier(
          vk::PipelineStageFlagBits::eFragmentShader,
          vk::PipelineStageFlagBits::eComputeShader,
          {},
          0,
          nullptr,
          0,
          nullptr,
          1,
          &imageMemoryBarrier
        );
        s_Pipelines.AtmospherePipeline.BindPipeline(commandBuffer.Get());
        s_Pipelines.AtmospherePipeline.BindDescriptorSets(commandBuffer.Get(), {s_AtmosphereDescriptorSet.Get()});
        commandBuffer.Dispatch((Window::GetWidth() + 8 - 1) / 8, (Window::GetHeight() + 8 - 1) / 8, 6);
      },
      clearValues,
      &VulkanContext::VulkanQueue.GraphicsQueue);
    //atmospherePass.AddToGraphCompute(renderGraph);

    RenderGraphPass compositePass(
      "Composite Pass",
      {&s_RendererContext.CompositeCommandBuffer},
      &s_Pipelines.CompositePipeline,
      {},
      [](VulkanCommandBuffer& commandBuffer, int32_t) {
        ZoneScopedN("Composite Pass");
        OX_TRACE_GPU(commandBuffer.Get(), "Composite Pass")
        s_Pipelines.CompositePipeline.BindPipeline(commandBuffer.Get());
        s_Pipelines.CompositePipeline.BindDescriptorSets(commandBuffer.Get(), {s_CompositeDescriptorSet.Get()});
        commandBuffer.Dispatch((Window::GetWidth() + 8 - 1) / 8, (Window::GetHeight() + 8 - 1) / 8, 1);
      },
      clearValues,
      &VulkanContext::VulkanQueue.GraphicsQueue);
    compositePass.AddToGraphCompute(renderGraph);

    RenderGraphPass ppPass({
      "PP Pass",
      {&s_RendererContext.PostProcessCommandBuffer},
      &s_Pipelines.PostProcessPipeline,
      {&s_FrameBuffers.PostProcessPassFB},
      [](VulkanCommandBuffer& commandBuffer, int32_t) {
        ZoneScopedN("PP Pass");
        OX_TRACE_GPU(commandBuffer.Get(), "PP Pass")
        commandBuffer.SetFlippedViwportWindow().SetScissorWindow();
        s_Pipelines.PostProcessPipeline.BindPipeline(commandBuffer.Get());
        s_Pipelines.PostProcessPipeline.BindDescriptorSets(commandBuffer.Get(), {s_PostProcessDescriptorSet.Get()});
        DrawFullscreenQuad(commandBuffer.Get(), true);
      },
      clearValues, &VulkanContext::VulkanQueue.GraphicsQueue
    });
    ppPass.AddToGraph(renderGraph);

    RenderGraphPass frustumPass(
      "Frustum Pass",
      {&s_RendererContext.FrustumCommandBuffer},
      {},
      {},
      [](const VulkanCommandBuffer& commandBuffer, int32_t) {
        ZoneScopedN("FrustumPass");
        OX_TRACE_GPU(commandBuffer.Get(), "Frustum Pass")
        s_Pipelines.FrustumGridPipeline.BindPipeline(commandBuffer.Get());
        s_Pipelines.FrustumGridPipeline.BindDescriptorSets(s_RendererContext.FrustumCommandBuffer.Get(), {s_ComputeDescriptorSet.Get()});
        commandBuffer.Dispatch(s_RendererData.UBO_PbrPassParams.numThreadGroups.x, s_RendererData.UBO_PbrPassParams.numThreadGroups.y, 1);
      },
      {},
      &VulkanContext::VulkanQueue.GraphicsQueue);
    //renderGraph.AddComputePass(frustumPass);

    RenderGraphPass lightListPass(
      "Light List Pass",
      {&s_RendererContext.LightListCommandBuffer},
      {},
      {},
      [](const VulkanCommandBuffer& commandBuffer, int32_t) {
        ZoneScopedN("Light List Pass");
        OX_TRACE_GPU(commandBuffer.Get(), "Light List Pass")
        std::vector<vk::BufferMemoryBarrier> barriers1;
        std::vector<vk::BufferMemoryBarrier> barriers2;
        static bool initalizedBarries = false;
        if (!initalizedBarries) {
          barriers1 = {
            s_RendererData.LightsBuffer.CreateMemoryBarrier(vk::AccessFlagBits::eShaderRead,
              vk::AccessFlagBits::eShaderWrite),
            s_RendererData.LighIndexBuffer.CreateMemoryBarrier(vk::AccessFlagBits::eShaderRead,
              vk::AccessFlagBits::eShaderWrite),
            s_RendererData.LighGridBuffer.CreateMemoryBarrier(vk::AccessFlagBits::eShaderRead,
              vk::AccessFlagBits::eShaderWrite),
          };

          barriers2 = {
            s_RendererData.LightsBuffer.CreateMemoryBarrier(vk::AccessFlagBits::eShaderWrite,
              vk::AccessFlagBits::eShaderRead),
            s_RendererData.LighIndexBuffer.CreateMemoryBarrier(vk::AccessFlagBits::eShaderWrite,
              vk::AccessFlagBits::eShaderRead),
            s_RendererData.LighGridBuffer.CreateMemoryBarrier(vk::AccessFlagBits::eShaderWrite,
              vk::AccessFlagBits::eShaderRead),
          };
          initalizedBarries = true;
        }

        commandBuffer.Get().pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
          vk::PipelineStageFlagBits::eComputeShader,
          vk::DependencyFlagBits::eByRegion,
          0,
          nullptr,
          (uint32_t)barriers1.size(),
          barriers1.data(),
          0,
          nullptr);
        s_Pipelines.LightListPipeline.BindPipeline(commandBuffer.Get());
        s_Pipelines.LightListPipeline.BindDescriptorSets(commandBuffer.Get(), {s_ComputeDescriptorSet.Get()});
        commandBuffer.Dispatch(s_RendererData.UBO_PbrPassParams.numThreadGroups.x,
          s_RendererData.UBO_PbrPassParams.numThreadGroups.y,
          1);
        commandBuffer.Get().pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
          vk::PipelineStageFlagBits::eComputeShader,
          vk::DependencyFlagBits::eByRegion,
          0,
          nullptr,
          (uint32_t)barriers2.size(),
          barriers2.data(),
          0,
          nullptr);
      },
      {},
      &VulkanContext::VulkanQueue.GraphicsQueue);

    //renderGraph.AddComputePass(lightListPass);
  }

  void VulkanRenderer::Init() {
    //Save/Load renderer config
    if (!RendererConfig::Get()->LoadConfig("renderer.oxconfig")) {
      RendererConfig::Get()->SaveConfig("renderer.oxconfig");
    }
    RendererConfig::Get()->ConfigChangeDispatcher.trigger(RendererConfig::ConfigChangeEvent{});

    const auto& LogicalDevice = VulkanContext::GetDevice();

    constexpr vk::DescriptorPoolSize poolSizes[] = {
      {vk::DescriptorType::eSampler, 50}, {vk::DescriptorType::eCombinedImageSampler, 50},
      {vk::DescriptorType::eSampledImage, 50}, {vk::DescriptorType::eStorageImage, 10},
      {vk::DescriptorType::eUniformTexelBuffer, 50}, {vk::DescriptorType::eStorageTexelBuffer, 10},
      {vk::DescriptorType::eUniformBuffer, 50}, {vk::DescriptorType::eStorageBuffer, 50},
      {vk::DescriptorType::eUniformBufferDynamic, 50}, {vk::DescriptorType::eStorageBufferDynamic, 10},
      {vk::DescriptorType::eInputAttachment, 50}
    };
    vk::DescriptorPoolCreateInfo poolInfo = {};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = 1000 * IM_ARRAYSIZE(poolSizes);
    poolInfo.poolSizeCount = static_cast<uint32_t>(IM_ARRAYSIZE(poolSizes));
    poolInfo.pPoolSizes = poolSizes;
    VulkanUtils::CheckResult(LogicalDevice.createDescriptorPool(&poolInfo, nullptr, &s_RendererContext.DescriptorPool));

    //Command Buffers
    vk::CommandPoolCreateInfo cmdPoolInfo;
    cmdPoolInfo.queueFamilyIndex = VulkanContext::VulkanQueue.graphicsQueueFamilyIndex;
    cmdPoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    s_RendererContext.CommandPool = VulkanContext::Context.Device.createCommandPool(cmdPoolInfo).value;

    SwapChain.SetVsync(RendererConfig::Get()->DisplayConfig.VSync, false);
    SwapChain.CreateSwapChain();

    s_RendererContext.TimelineCommandBuffer.CreateBuffer();

    s_RendererContext.PostProcessCommandBuffer.CreateBuffer();
    s_RendererContext.PBRPassCommandBuffer.CreateBuffer();
    s_RendererContext.BloomPassCommandBuffer.CreateBuffer();
    s_RendererContext.SSRCommandBuffer.CreateBuffer();
    s_RendererContext.FrustumCommandBuffer.CreateBuffer();
    s_RendererContext.LightListCommandBuffer.CreateBuffer();
    s_RendererContext.DepthPassCommandBuffer.CreateBuffer();
    s_RendererContext.SSAOCommandBuffer.CreateBuffer();
    s_RendererContext.DirectShadowCommandBuffer.CreateBuffer();
    s_RendererContext.CompositeCommandBuffer.CreateBuffer();
    s_RendererContext.AtmosphereCommandBuffer.CreateBuffer();
    s_RendererContext.DepthOfFieldCommandBuffer.CreateBuffer();

    vk::DescriptorSetLayoutBinding binding[1];
    binding[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    binding[0].descriptorCount = 1;
    binding[0].stageFlags = vk::ShaderStageFlagBits::eFragment;

    vk::DescriptorSetLayoutCreateInfo info = {};
    info.bindingCount = 1;
    info.pBindings = binding;
    VulkanUtils::CheckResult(
      LogicalDevice.createDescriptorSetLayout(&info, nullptr, &s_RendererData.ImageDescriptorSetLayout));

    s_RendererData.SkyboxBuffer.CreateBuffer(vk::BufferUsageFlagBits::eUniformBuffer,
      vk::MemoryPropertyFlagBits::eHostVisible |
      vk::MemoryPropertyFlagBits::eHostCoherent,
      sizeof RendererData::UBO_VS,
      &s_RendererData.UBO_VS).Map();

    s_RendererData.ParametersBuffer.CreateBuffer(vk::BufferUsageFlagBits::eUniformBuffer,
      vk::MemoryPropertyFlagBits::eHostVisible |
      vk::MemoryPropertyFlagBits::eHostCoherent,
      sizeof RendererData::UBO_PbrPassParams,
      &s_RendererData.UBO_PbrPassParams).Map();

    s_RendererData.VSBuffer.CreateBuffer(vk::BufferUsageFlagBits::eUniformBuffer,
      vk::MemoryPropertyFlagBits::eHostVisible |
      vk::MemoryPropertyFlagBits::eHostCoherent,
      sizeof RendererData::UBO_VS,
      &s_RendererData.UBO_VS).Map();

    s_RendererData.LightsBuffer.CreateBuffer(
      vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
      sizeof(LightingData)).Map().SetOnUpdate([] {
      UpdateLightingData();
    }).Sink<LightChangeEvent>(s_LightBufferDispatcher);

    s_RendererData.FrustumBuffer.CreateBuffer(
      vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
      sizeof RendererData::Frustums,
      &s_RendererData.Frustums).Map();

    s_RendererData.LighGridBuffer.CreateBuffer(vk::BufferUsageFlagBits::eStorageBuffer,
      vk::MemoryPropertyFlagBits::eHostVisible |
      vk::MemoryPropertyFlagBits::eHostCoherent,
      sizeof(uint32_t) * MAX_NUM_FRUSTUMS * MAX_NUM_LIGHTS_PER_TILE).Map();

    s_RendererData.LighIndexBuffer.CreateBuffer(vk::BufferUsageFlagBits::eStorageBuffer,
      vk::MemoryPropertyFlagBits::eHostVisible |
      vk::MemoryPropertyFlagBits::eHostCoherent,
      sizeof(uint32_t) * MAX_NUM_FRUSTUMS).Map();

    s_RendererData.SSRBuffer.CreateBuffer(vk::BufferUsageFlagBits::eUniformBuffer,
      vk::MemoryPropertyFlagBits::eHostVisible |
      vk::MemoryPropertyFlagBits::eHostCoherent,
      sizeof RendererData::UBO_SSR).Map().SetOnUpdate([] {
      s_RendererData.UBO_SSR.Samples = RendererConfig::Get()->SSRConfig.Samples;
      s_RendererData.UBO_SSR.MaxDist = RendererConfig::Get()->SSRConfig.MaxDist;
      s_RendererData.SSRBuffer.Copy(&s_RendererData.UBO_SSR, sizeof s_RendererData.UBO_SSR);
    }).Sink<RendererConfig::ConfigChangeEvent>(RendererConfig::Get()->ConfigChangeDispatcher);

    {
      s_RendererData.UBO_Atmosphere.InvViews[0] = glm::inverse(Camera::GenerateViewMatrix({}, Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f))); // PositiveX
      s_RendererData.UBO_Atmosphere.InvViews[1] = glm::inverse(Camera::GenerateViewMatrix({}, Vec3(-1.0f, 0.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f))); // NegativeX

      s_RendererData.UBO_Atmosphere.InvViews[2] = glm::inverse(Camera::GenerateViewMatrix({}, Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f))); // PositiveY
      s_RendererData.UBO_Atmosphere.InvViews[3] = glm::inverse(Camera::GenerateViewMatrix({}, Vec3(0.0f, -1.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f))); // NegativeY

      s_RendererData.UBO_Atmosphere.InvViews[4] = glm::inverse(Camera::GenerateViewMatrix({}, Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, -1.0f, 0.0f))); // PositiveZ
      s_RendererData.UBO_Atmosphere.InvViews[5] = glm::inverse(Camera::GenerateViewMatrix({}, Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, -1.0f, 0.0f))); // NegativeZ
      s_RendererData.AtmosphereBuffer.CreateBuffer(vk::BufferUsageFlagBits::eUniformBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent,
        sizeof RendererData::UBO_Atmosphere).Map();
    }

    s_RendererData.SSAOBuffer.CreateBuffer(vk::BufferUsageFlagBits::eUniformBuffer,
      vk::MemoryPropertyFlagBits::eHostVisible |
      vk::MemoryPropertyFlagBits::eHostCoherent,
      sizeof RendererData::UBO_SSAOParams,
      &s_RendererData.UBO_SSAOParams).Map().SetOnUpdate([] {
      s_RendererData.UBO_SSAOParams.radius = RendererConfig::Get()->SSAOConfig.Radius;
      s_RendererData.SSAOBuffer.Copy(&s_RendererData.UBO_SSAOParams, sizeof s_RendererData.UBO_SSAOParams);
    }).Sink<RendererConfig::ConfigChangeEvent>(RendererConfig::Get()->ConfigChangeDispatcher);

    //Postprocessing buffer
    {
      s_RendererData.PostProcessBuffer.CreateBuffer(vk::BufferUsageFlagBits::eUniformBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible,
        sizeof RendererData::UBO_PostProcessParams,
        &s_RendererData.UBO_PostProcessParams).Map().SetOnUpdate([] {
        s_RendererData.UBO_PostProcessParams.Tonemapper = RendererConfig::Get()->ColorConfig.Tonemapper;
        s_RendererData.UBO_PostProcessParams.Exposure = RendererConfig::Get()->ColorConfig.Exposure;
        s_RendererData.UBO_PostProcessParams.Gamma = RendererConfig::Get()->ColorConfig.Gamma;
        s_RendererData.UBO_PostProcessParams.EnableSSAO = RendererConfig::Get()->SSAOConfig.Enabled;
        s_RendererData.UBO_PostProcessParams.EnableBloom = RendererConfig::Get()->BloomConfig.Enabled;
        s_RendererData.UBO_PostProcessParams.EnableSSR = RendererConfig::Get()->SSRConfig.Enabled;
        s_RendererData.PostProcessBuffer.Copy(&s_RendererData.UBO_PostProcessParams, sizeof s_RendererData.UBO_PostProcessParams);
      }).Sink<RendererConfig::ConfigChangeEvent>(RendererConfig::Get()->ConfigChangeDispatcher);
    }

    //Direct shadow buffer
    {
      s_RendererData.DirectShadowBuffer.CreateBuffer(vk::BufferUsageFlagBits::eUniformBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible,
        sizeof RendererData::UBO_DirectShadow,
        &s_RendererData.UBO_DirectShadow).Map();
    }

    //Create Triangle Buffers for rendering a single triangle.
    {
      std::vector<RendererData::Vertex> vertexBuffer = {
        {{-1.0f, -1.0f, 0.0f}, {}, {0.0f, 1.0f}}, {{-1.0f, 3.0f, 0.0f}, {}, {0.0f, -1.0f}},
        {{3.0f, -1.0f, 0.0f}, {}, {2.0f, 1.0f}},
      };

      VulkanBuffer vertexStaging;

      uint64_t vBufferSize = (uint32_t)vertexBuffer.size() * sizeof(RendererData::Vertex);

      vertexStaging.CreateBuffer(vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        vBufferSize,
        vertexBuffer.data());

      s_TriangleVertexBuffer.CreateBuffer(
        vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        vBufferSize);

      SubmitOnce([&vBufferSize, &vertexStaging](const VulkanCommandBuffer copyCmd) {
        vk::BufferCopy copyRegion{};

        copyRegion.size = vBufferSize;
        vertexStaging.CopyTo(s_TriangleVertexBuffer.Get(), copyCmd.Get(), copyRegion);
      });

      vertexStaging.Destroy();
    }

    //Lights data
    s_PointLightsData.reserve(MAX_NUM_LIGHTS);

    //Mesh data
    s_MeshDrawList.reserve(MAX_NUM_MESHES);

    Resources::InitEngineResources();

    s_SkyboxCube.LoadFromFile(Resources::GetResourcesPath("Objects/cube.gltf").string(), Mesh::FlipY | Mesh::DontCreateMaterials);

    VulkanImageDescription CubeMapDesc{};
    //temp fail-safe until we have an actual atmosphere or load the sky from scene
    if (std::filesystem::exists(Resources::GetResourcesPath("HDRs/industrial_sky.ktx2").string()))
      CubeMapDesc.Path = Resources::GetResourcesPath("HDRs/industrial_sky.ktx2").string();
    else
      CubeMapDesc.Path = Resources::GetResourcesPath("HDRs/HDRs/belfast_sunset.ktx2").string();
    CubeMapDesc.Type = ImageType::TYPE_CUBE;
    s_Resources.CubeMap.Create(CubeMapDesc);

    CreateGraphicsPipelines();
    CreateFramebuffers();

    s_QuadDescriptorSet.CreateFromPipeline(s_Pipelines.QuadPipeline);
    s_SkyboxDescriptorSet.CreateFromPipeline(s_Pipelines.SkyboxPipeline);
    s_ComputeDescriptorSet.CreateFromPipeline(s_Pipelines.LightListPipeline);
    s_SSAODescriptorSet.CreateFromPipeline(s_Pipelines.SSAOPassPipeline);
    s_SSAOBlurDescriptorSet.CreateFromPipeline(s_Pipelines.GaussianBlurPipeline);
    s_PostProcessDescriptorSet.CreateFromPipeline(s_Pipelines.PostProcessPipeline);
    s_BloomDescriptorSet.CreateFromPipeline(s_Pipelines.BloomPipeline);
    s_DepthDescriptorSet.CreateFromPipeline(s_Pipelines.DepthPrePassPipeline);
    s_ShadowDepthDescriptorSet.CreateFromPipeline(s_Pipelines.DirectShadowDepthPipeline);
    s_SSRDescriptorSet.CreateFromPipeline(s_Pipelines.SSRPipeline);
    s_CompositeDescriptorSet.CreateFromPipeline(s_Pipelines.CompositePipeline);
    s_AtmosphereDescriptorSet.CreateFromPipeline(s_Pipelines.AtmospherePipeline);
    s_DepthOfFieldDescriptorSet.CreateFromPipeline(s_Pipelines.DepthOfFieldPipeline);

    GeneratePrefilter();

    UpdateSkyboxDescriptorSets();
    UpdateComputeDescriptorSets();
    UpdateSSAODescriptorSets();
    s_FrameBuffers.PBRPassFB.GetDescription().OnResize();
    s_FrameBuffers.PostProcessPassFB.GetDescription().OnResize();
    for (auto& fb : s_FrameBuffers.DirectionalCascadesFB)
      fb.GetDescription().OnResize();

    ShaderLibrary::UnloadShaders();

    s_RendererContext.Initialized = true;

    //Render graph
    InitRenderGraph();

    s_RendererConfig.ConfigChangeDispatcher.trigger(RendererConfig::ConfigChangeEvent{});

#if GPU_PROFILER_ENABLED
    //Initalize tracy profiling
    const VkPhysicalDevice physicalDevice = VulkanContext::Context.PhysicalDevice;
    TracyProfiler::InitTracyForVulkan(physicalDevice,
      LogicalDevice,
      VulkanContext::VulkanQueue.GraphicsQueue,
      s_RendererContext.TimelineCommandBuffer.Get());
#endif
  }

  void VulkanRenderer::Shutdown() {
    RendererConfig::Get()->SaveConfig("renderer.oxconfig");
#if GPU_PROFILER_ENABLED
    TracyProfiler::DestroyContext();
#endif
  }

  void VulkanRenderer::Submit(const std::function<void()>& submitFunc) {
    const auto& LogicalDevice = VulkanContext::Context.Device;
    const auto& CommandBuffer = SwapChain.GetCommandBuffer();
    const auto& GraphicsQueue = VulkanContext::VulkanQueue.GraphicsQueue;

    vk::CommandBufferBeginInfo beginInfo = {};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

    vk::SubmitInfo endInfo;
    endInfo.pCommandBuffers = &CommandBuffer.Get();
    endInfo.commandBufferCount = 1;

    LogicalDevice.resetCommandPool(s_RendererContext.CommandPool);
    CommandBuffer.Begin(beginInfo);
    submitFunc();
    CommandBuffer.End();
    VulkanUtils::CheckResult(GraphicsQueue.submit(endInfo));
    WaitDeviceIdle();
  }

  void VulkanRenderer::SubmitOnce(const std::function<void(VulkanCommandBuffer& cmdBuffer)>& submitFunc) {
    VulkanCommandBuffer cmdBuffer;
    cmdBuffer.CreateBuffer();
    cmdBuffer.Begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    submitFunc(cmdBuffer);
    cmdBuffer.End();
    cmdBuffer.FlushBuffer();
    cmdBuffer.FreeBuffer();
  }

  void VulkanRenderer::SubmitQueue(const VulkanCommandBuffer& commandBuffer) {
    vk::SubmitInfo end_info = {};
    end_info.commandBufferCount = 1;
    end_info.pCommandBuffers = &commandBuffer.Get();

    VulkanUtils::CheckResult(VulkanContext::VulkanQueue.GraphicsQueue.submit(1, &end_info, VK_NULL_HANDLE));
    WaitDeviceIdle();
  }

  void VulkanRenderer::SubmitLights(std::vector<Entity>&& lights) {
    s_SceneLights = std::move(lights);
    s_LightBufferDispatcher.trigger(LightChangeEvent{});
  }

  void VulkanRenderer::SubmitSkyLight(const Entity& entity) {
    s_Skylight = entity;
    const auto& skyLight = s_Skylight.GetComponent<SkyLightComponent>();
    s_RendererData.UBO_PbrPassParams.lodBias = skyLight.CubemapLodBias;
    if (skyLight.Cubemap && skyLight.Cubemap->LoadCallback) {
      s_Resources.CubeMap = *skyLight.Cubemap;
      s_ForceUpdateMaterials = true;
      skyLight.Cubemap->LoadCallback = false;
    }
  }

  void VulkanRenderer::SubmitMesh(Mesh& mesh, const Mat4& transform, std::vector<Ref<Material>>& materials, uint32_t submeshIndex) {
    s_MeshDrawList.emplace_back(mesh, transform, materials, submeshIndex);
  }

  void VulkanRenderer::SubmitQuad(const Mat4& transform, const Ref<VulkanImage>& image, const Vec4& color) {
    s_QuadDrawList.emplace_back(transform, image, color);
  }

  const VulkanImage& VulkanRenderer::GetFinalImage() {
    return s_FrameBuffers.PostProcessPassFB.GetImage()[0];
  }

  void VulkanRenderer::SetCamera(Camera& camera) {
    s_RendererContext.CurrentCamera = &camera;
  }

  void VulkanRenderer::RenderNode(const Mesh::Node* node,
                                  const vk::CommandBuffer& commandBuffer,
                                  const VulkanPipeline& pipeline,
                                  const std::function<bool(Mesh::Primitive* prim)>& perMeshFunc) {
    for (const auto& part : node->Primitives) {
      if (!perMeshFunc(part))
        continue;
      commandBuffer.drawIndexed(part->indexCount, 1, part->firstIndex, 0, 0);
    }
    for (const auto& child : node->Children) {
      RenderNode(child, commandBuffer, pipeline, perMeshFunc);
    }
  }

  void VulkanRenderer::RenderMesh(const MeshData& mesh,
                                  const vk::CommandBuffer& commandBuffer,
                                  const VulkanPipeline& pipeline,
                                  const std::function<bool(Mesh::Primitive* prim)>& perMeshFunc) {
    pipeline.BindPipeline(commandBuffer);

    if (mesh.MeshGeometry.ShouldUpdate || s_ForceUpdateMaterials) {
      mesh.MeshGeometry.UpdateMaterials();
      mesh.MeshGeometry.ShouldUpdate = false;
      return;
    }

    constexpr vk::DeviceSize offsets[1] = {0};
    commandBuffer.bindVertexBuffers(0, mesh.MeshGeometry.VerticiesBuffer.Get(), offsets);
    commandBuffer.bindIndexBuffer(mesh.MeshGeometry.IndiciesBuffer.Get(), 0, vk::IndexType::eUint32);

    RenderNode(mesh.MeshGeometry.LinearNodes[mesh.SubmeshIndex], commandBuffer, pipeline, perMeshFunc);
  }

  void VulkanRenderer::Draw() {
    ZoneScoped;
    if (!s_RendererContext.CurrentCamera) {
      OX_CORE_ERROR("Renderer couldn't find a camera!");
      return;
    }

    UpdateUniformBuffers();

    if (!s_RendererContext.RenderGraph.Update(SwapChain, &SwapChain.CurrentFrame)) {
      return;
    }

    SwapChain.SubmitPass([](const VulkanCommandBuffer& commandBuffer) {
      ZoneScopedN("Swapchain pass");
      OX_TRACE_GPU(commandBuffer.Get(), "Swapchain Pass")
      s_Pipelines.QuadPipeline.BindPipeline(commandBuffer.Get());
      s_Pipelines.QuadPipeline.BindDescriptorSets(commandBuffer.Get(), {s_QuadDescriptorSet.Get()});
      DrawFullscreenQuad(commandBuffer.Get());
      //UI pass
      Application::Get().GetImGuiLayer()->RenderDrawData(commandBuffer.Get(), s_Pipelines.UIPipeline.Get());
    })->Submit()->Present();
  }

  void VulkanRenderer::DrawFullscreenQuad(const vk::CommandBuffer& commandBuffer, const bool bindVertex) {
    ZoneScoped;
    if (bindVertex) {
      constexpr vk::DeviceSize offsets[1] = {0};
      commandBuffer.bindVertexBuffers(0, s_TriangleVertexBuffer.Get(), offsets);
      commandBuffer.draw(3, 1, 0, 0);
    }
    else {
      commandBuffer.draw(3, 1, 0, 0);
    }
  }

  void VulkanRenderer::DrawQuad(const vk::CommandBuffer& commandBuffer) {
    constexpr vk::DeviceSize offsets[1] = {0};
    commandBuffer.bindVertexBuffers(0, s_QuadVertexBuffer.Get(), offsets);
    commandBuffer.draw(MAX_PARTICLE_COUNT, 1, 0, 0);
  }

  void VulkanRenderer::OnResize() {
    SwapChain.Resizing = true;
  }

  void VulkanRenderer::WaitDeviceIdle() {
    const auto& LogicalDevice = VulkanContext::Context.Device;
    VulkanUtils::CheckResult(LogicalDevice.waitIdle());
  }

  void VulkanRenderer::WaitGraphicsQueueIdle() {
    VulkanUtils::CheckResult(VulkanContext::VulkanQueue.GraphicsQueue.waitIdle());
  }
}
