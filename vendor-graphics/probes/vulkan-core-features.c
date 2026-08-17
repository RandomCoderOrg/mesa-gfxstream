/* Report the core Vulkan feature bits used by Zink's base requirement gate. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>

int
main(void)
{
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vulkan-core-features",
      .apiVersion = VK_API_VERSION_1_0,
   };
   const VkInstanceCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = vkCreateInstance(&create, NULL, &instance);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance=%d\n", result);
      return 1;
   }

   uint32_t count = 0;
   result = vkEnumeratePhysicalDevices(instance, &count, NULL);
   if (result != VK_SUCCESS || !count) {
      fprintf(stderr, "vkEnumeratePhysicalDevices=%d count=%u\n", result, count);
      vkDestroyInstance(instance, NULL);
      return 2;
   }

   VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
   if (!devices) {
      vkDestroyInstance(instance, NULL);
      return 3;
   }
   result = vkEnumeratePhysicalDevices(instance, &count, devices);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkEnumeratePhysicalDevices=%d\n", result);
      free(devices);
      vkDestroyInstance(instance, NULL);
      return 4;
   }
   for (uint32_t i = 0; i < count; ++i) {
      VkPhysicalDeviceProperties properties;
      VkPhysicalDeviceFeatures features;
      vkGetPhysicalDeviceProperties(devices[i], &properties);
      vkGetPhysicalDeviceFeatures(devices[i], &features);
      printf("device[%u]=%s api=%u.%u.%u\n", i, properties.deviceName,
             VK_VERSION_MAJOR(properties.apiVersion),
             VK_VERSION_MINOR(properties.apiVersion),
             VK_VERSION_PATCH(properties.apiVersion));
      printf("fillModeNonSolid=%u shaderClipDistance=%u\n",
             features.fillModeNonSolid, features.shaderClipDistance);
   }

   free(devices);
   vkDestroyInstance(instance, NULL);
   return 0;
}
