#pragma once
#include <future>

#include "Vulkan/VulkanShader.h"
#include "Core/Base.h"

namespace Oxylus {
  class ShaderLibrary {
  public:
    static Ref<VulkanShader> CreateShader(const ShaderCI& shaderCreateInfo);
    static std::future<Ref<VulkanShader>> CreateShaderAsync(const ShaderCI& shaderCreateInfo);
    static void AddShader(const Ref<VulkanShader>& shader);
    static void RemoveShader(const std::string& name);

    static const std::unordered_map<std::string, Ref<VulkanShader>>& GetShaders() { return s_Shaders; }

    static Ref<VulkanShader> GetShader(const std::string& name);
    static void UnloadShaders();

    static std::unordered_map<std::string, Ref<VulkanShader>> s_Shaders;
  };
}