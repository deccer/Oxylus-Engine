#pragma once
#include <vulkan/vulkan.hpp>

#include "Core/Application.h"
#include <vk_mem_alloc.h>

namespace Oxylus {
  class VulkanContext {
  public:
    struct VkContext {
      vk::Instance Instance;
      vk::SurfaceKHR Surface;
      std::vector<vk::PhysicalDevice> PhysicalDevices;
      vk::PhysicalDevice PhysicalDevice;
      vk::PhysicalDeviceFeatures DeviceFeatures;
      vk::PhysicalDeviceProperties DeviceProperties;
      vk::PhysicalDeviceMemoryProperties DeviceMemoryProperties;
      vk::Device Device;
      VmaAllocator Allocator;
      vk::DynamicLoader DynamicLoader;
    };

    struct VkQueue {
      vk::Queue GraphicsQueue;
      vk::Queue PresentQueue;
      vk::Queue ComputeQueue;
      std::vector<vk::QueueFamilyProperties> queueFamilyProperties;
      uint32_t graphicsQueueFamilyIndex;
      uint32_t presentQueueFamilyIndex;
      uint32_t computeQueueFamilyIndex; //TODO:
    };

    static void CreateContext(const AppSpec& spec);

    static vk::Instance& GetInstance() { return Context.Instance; }
    static VmaAllocator& GetAllocator() { return Context.Allocator; }
    static vk::Device& GetDevice() { return Context.Device; }
    static vk::PhysicalDevice& GetPhysicalDevice() { return Context.PhysicalDevice; }
    static vk::DynamicLoader& GetDynamicLoader() { return Context.DynamicLoader; }

    static VkContext Context;
    static VkQueue VulkanQueue;
  };
}
