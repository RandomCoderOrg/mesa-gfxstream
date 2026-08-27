/*
 * Copyright 2026 Google
 * SPDX-License-Identifier: MIT
 */

#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using PfnPresent = VkResult(VKAPI_PTR*)(VkImage, int, int*);

static void fail(const char* operation, VkResult result = VK_ERROR_UNKNOWN) {
    std::fprintf(stderr, "[fail] %s: %d\n", operation, result);
    std::exit(1);
}

static void stage(const char* operation) { std::printf("[stage] %s\n", operation); }

template <typename T>
static T instance_proc(PFN_vkGetInstanceProcAddr getProc, VkInstance instance, const char* name) {
    auto proc = reinterpret_cast<T>(getProc(instance, name));
    if (!proc) fail(name);
    return proc;
}

template <typename T>
static T device_proc(PFN_vkGetDeviceProcAddr getProc, VkDevice device, const char* name) {
    auto proc = reinterpret_cast<T>(getProc(device, name));
    if (!proc) fail(name);
    return proc;
}

int main(int argc, char** argv) {
    // This probe is normally driven through adb -> PRoot. Keep each operation
    // visible even when the host dies before the process can flush stdio.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    const char* icdPath = argc > 1 ? argv[1] : "./libvulkan_gfxstream.so";
    void* library = dlopen(icdPath, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::fprintf(stderr, "[fail] dlopen %s: %s\n", icdPath, dlerror());
        return 1;
    }

    auto getInstanceProc =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vk_icdGetInstanceProcAddr"));
    if (!getInstanceProc) fail("vk_icdGetInstanceProcAddr");
    auto createInstance =
        instance_proc<PFN_vkCreateInstance>(getInstanceProc, VK_NULL_HANDLE, "vkCreateInstance");
    auto enumerateInstanceVersion = instance_proc<PFN_vkEnumerateInstanceVersion>(
        getInstanceProc, VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t reportedVersion = VK_API_VERSION_1_0;
    VkResult result = enumerateInstanceVersion(&reportedVersion);
    std::printf("[guest] vkEnumerateInstanceVersion=%d api=%u.%u.%u\n", result,
                VK_API_VERSION_MAJOR(reportedVersion), VK_API_VERSION_MINOR(reportedVersion),
                VK_API_VERSION_PATCH(reportedVersion));
    if (result != VK_SUCCESS) fail("vkEnumerateInstanceVersion", result);

    const VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "uDroid Kumquat presenter probe",
        .applicationVersion = 1,
        .pEngineName = "none",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    const VkInstanceCreateInfo instanceInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
    };
    VkInstance instance = VK_NULL_HANDLE;
    stage("vkCreateInstance");
    result = createInstance(&instanceInfo, nullptr, &instance);
    if (result != VK_SUCCESS) fail("vkCreateInstance", result);

    auto enumeratePhysicalDevices = instance_proc<PFN_vkEnumeratePhysicalDevices>(
        getInstanceProc, instance, "vkEnumeratePhysicalDevices");
    auto getPhysicalDeviceProperties = instance_proc<PFN_vkGetPhysicalDeviceProperties>(
        getInstanceProc, instance, "vkGetPhysicalDeviceProperties");
    auto getPhysicalDeviceMemoryProperties = instance_proc<PFN_vkGetPhysicalDeviceMemoryProperties>(
        getInstanceProc, instance, "vkGetPhysicalDeviceMemoryProperties");
    auto getPhysicalDeviceQueueFamilyProperties =
        instance_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            getInstanceProc, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto enumerateDeviceExtensions = instance_proc<PFN_vkEnumerateDeviceExtensionProperties>(
        getInstanceProc, instance, "vkEnumerateDeviceExtensionProperties");
    auto createDevice =
        instance_proc<PFN_vkCreateDevice>(getInstanceProc, instance, "vkCreateDevice");
    auto getDeviceProc =
        instance_proc<PFN_vkGetDeviceProcAddr>(getInstanceProc, instance, "vkGetDeviceProcAddr");
    auto privatePresent = instance_proc<PfnPresent>(getInstanceProc, VK_NULL_HANDLE,
                                                    "gfxstream_kumquat_present_image");

    uint32_t physicalCount = 0;
    if (enumeratePhysicalDevices(instance, &physicalCount, nullptr) != VK_SUCCESS ||
        physicalCount == 0) {
        fail("vkEnumeratePhysicalDevices");
    }
    std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
    if (enumeratePhysicalDevices(instance, &physicalCount, physicalDevices.data()) != VK_SUCCESS) {
        fail("vkEnumeratePhysicalDevices(list)");
    }
    VkPhysicalDevice physicalDevice = physicalDevices.front();
    VkPhysicalDeviceProperties properties = {};
    getPhysicalDeviceProperties(physicalDevice, &properties);
    std::printf("[gpu] %s api=%u.%u.%u\n", properties.deviceName,
                VK_API_VERSION_MAJOR(properties.apiVersion),
                VK_API_VERSION_MINOR(properties.apiVersion),
                VK_API_VERSION_PATCH(properties.apiVersion));

    uint32_t queueFamilyCount = 0;
    getPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    getPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());
    uint32_t queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamily = i;
            break;
        }
    }
    if (queueFamily == UINT32_MAX) fail("graphics queue family");

    uint32_t extensionCount = 0;
    enumerateDeviceExtensions(physicalDevice, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    enumerateDeviceExtensions(physicalDevice, nullptr, &extensionCount, extensions.data());
    bool hasExternalFence = false;
    bool hasExternalFenceFd = false;
    bool hasExternalMemory = false;
    bool hasExternalMemoryFd = false;
    bool hasExternalMemoryDmaBuf = false;
    for (const auto& extension : extensions) {
        hasExternalFence |=
            !std::strcmp(extension.extensionName, VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME);
        hasExternalFenceFd |=
            !std::strcmp(extension.extensionName, VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME);
        hasExternalMemory |=
            !std::strcmp(extension.extensionName, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
        hasExternalMemoryFd |=
            !std::strcmp(extension.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        hasExternalMemoryDmaBuf |=
            !std::strcmp(extension.extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    }
    if (!hasExternalFence || !hasExternalFenceFd) fail("external sync-fd fence extensions");
    if (!hasExternalMemoryFd || !hasExternalMemoryDmaBuf) {
        fail("external DMA-BUF memory extensions");
    }
    std::printf("[guest] external memory: core=%s fd=yes dma-buf=yes\n",
                hasExternalMemory ? "extension" : "Vulkan 1.1");

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queueInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queueFamily,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char* deviceExtensions[] = {
        VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    const VkDeviceCreateInfo deviceInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = 4,
        .ppEnabledExtensionNames = deviceExtensions,
    };
    VkDevice device = VK_NULL_HANDLE;
    stage("vkCreateDevice");
    result = createDevice(physicalDevice, &deviceInfo, nullptr, &device);
    if (result != VK_SUCCESS) fail("vkCreateDevice", result);

#define LOAD_DEVICE(name) auto name = device_proc<PFN_##name>(getDeviceProc, device, #name)
    LOAD_DEVICE(vkGetDeviceQueue);
    LOAD_DEVICE(vkCreateImage);
    LOAD_DEVICE(vkGetImageMemoryRequirements);
    LOAD_DEVICE(vkAllocateMemory);
    LOAD_DEVICE(vkBindImageMemory);
    LOAD_DEVICE(vkCreateCommandPool);
    LOAD_DEVICE(vkAllocateCommandBuffers);
    LOAD_DEVICE(vkResetCommandBuffer);
    LOAD_DEVICE(vkBeginCommandBuffer);
    LOAD_DEVICE(vkCmdPipelineBarrier);
    LOAD_DEVICE(vkCmdClearColorImage);
    LOAD_DEVICE(vkEndCommandBuffer);
    LOAD_DEVICE(vkCreateFence);
    LOAD_DEVICE(vkQueueSubmit);
    LOAD_DEVICE(vkGetFenceFdKHR);
    LOAD_DEVICE(vkDestroyFence);
    LOAD_DEVICE(vkQueueWaitIdle);
    LOAD_DEVICE(vkDestroyCommandPool);
    LOAD_DEVICE(vkDestroyImage);
    LOAD_DEVICE(vkFreeMemory);
    LOAD_DEVICE(vkDestroyDevice);
#undef LOAD_DEVICE

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    const VkExtent3D extent = {.width = 720, .height = 1280, .depth = 1};
    const VkExternalMemoryImageCreateInfo externalImageInfo = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    const VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &externalImageInfo,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = extent,
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    stage("vkCreateImage(external DMA-BUF)");
    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) fail("vkCreateImage");

    VkMemoryRequirements memoryRequirements = {};
    vkGetImageMemoryRequirements(device, image, &memoryRequirements);
    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    getPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if (memoryRequirements.memoryTypeBits & (1u << i)) {
            memoryType = i;
            break;
        }
    }
    if (memoryType == UINT32_MAX) fail("image memory type");
    const VkMemoryDedicatedAllocateInfo dedicatedInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = image,
    };
    const VkExportMemoryAllocateInfo exportInfo = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicatedInfo,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    const VkMemoryAllocateInfo allocationInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &exportInfo,
        .allocationSize = memoryRequirements.size,
        .memoryTypeIndex = memoryType,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    stage("vkAllocateMemory(export DMA-BUF)");
    if (vkAllocateMemory(device, &allocationInfo, nullptr, &memory) != VK_SUCCESS) {
        fail("vkAllocateMemory");
    }
    stage("vkBindImageMemory");
    if (vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) fail("vkBindImageMemory");

    const VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamily,
    };
    VkCommandPool commandPool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        fail("vkCreateCommandPool");
    }
    const VkCommandBufferAllocateInfo commandInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &commandInfo, &commandBuffer) != VK_SUCCESS) {
        fail("vkAllocateCommandBuffers");
    }

    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    stage("render and present loop");
    for (uint32_t frame = 0; frame < 180; ++frame) {
        vkResetCommandBuffer(commandBuffer, 0);
        const VkCommandBufferBeginInfo beginInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(commandBuffer, &beginInfo);
        const VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
                                 ? VkAccessFlags{0}
                                 : VkAccessFlags{VK_ACCESS_TRANSFER_WRITE_BIT},
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = oldLayout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        const float phase = static_cast<float>(frame % 90) / 89.0f;
        const VkClearColorValue color = {{phase, 0.15f, 1.0f - phase, 1.0f}};
        vkCmdClearColorImage(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                             &barrier.subresourceRange);
        vkEndCommandBuffer(commandBuffer);
        oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        const VkExportFenceCreateInfo exportInfo = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        const VkFenceCreateInfo fenceInfo = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = &exportInfo,
        };
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            fail("vkCreateFence");
        }
        const VkSubmitInfo submitInfo = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBuffer,
        };
        if (vkQueueSubmit(queue, 1, &submitInfo, fence) != VK_SUCCESS) fail("vkQueueSubmit");
        const VkFenceGetFdInfoKHR fdInfo = {
            .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
            .fence = fence,
            .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        int acquireFence = -1;
        if (vkGetFenceFdKHR(device, &fdInfo, &acquireFence) != VK_SUCCESS || acquireFence < 0) {
            fail("vkGetFenceFdKHR");
        }
        int releaseFence = -1;
        result = privatePresent(image, acquireFence, &releaseFence);
        if (result != VK_SUCCESS || releaseFence < 0) fail("gfxstream Kumquat present", result);
        pollfd wait = {.fd = releaseFence, .events = POLLIN};
        if (poll(&wait, 1, 5000) != 1) fail("presenter release fence wait");
        close(releaseFence);
        vkDestroyFence(device, fence, nullptr);
        if ((frame + 1) % 30 == 0) std::printf("[frame] %u/180\n", frame + 1);
        usleep(16667);
    }

    vkQueueWaitIdle(queue);
    if (const char* holdValue = std::getenv("UDROID_GFXSTREAM_VISUAL_HOLD_SECONDS")) {
        const unsigned long requested = std::strtoul(holdValue, nullptr, 10);
        const unsigned int seconds = static_cast<unsigned int>(std::min(requested, 60UL));
        if (seconds > 0) {
            std::printf("[hold] keeping the final frame alive for %u seconds\n", seconds);
            std::fflush(stdout);
            sleep(seconds);
        }
    }
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyImage(device, image, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr);
    auto destroyInstance =
        instance_proc<PFN_vkDestroyInstance>(getInstanceProc, instance, "vkDestroyInstance");
    destroyInstance(instance, nullptr);
    dlclose(library);
    std::puts("[pass] 180 explicit-fence frames presented");
    return 0;
}
