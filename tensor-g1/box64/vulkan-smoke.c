#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

static void fail(const char *message)
{
   fprintf(stderr, "VULKAN_SMOKE_FAIL: %s\n", message);
   exit(1);
}

int main(void)
{
   void *library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!library)
      fail(dlerror());

   PFN_vkGetInstanceProcAddr get_instance_proc_addr =
      (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
   if (!get_instance_proc_addr)
      fail("vkGetInstanceProcAddr is unavailable");

   PFN_vkEnumerateInstanceVersion enumerate_instance_version =
      (PFN_vkEnumerateInstanceVersion)get_instance_proc_addr(
         VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
   uint32_t loader_version = VK_API_VERSION_1_0;
   if (enumerate_instance_version &&
       enumerate_instance_version(&loader_version) != VK_SUCCESS)
      fail("vkEnumerateInstanceVersion failed");

   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)get_instance_proc_addr(VK_NULL_HANDLE,
                                                   "vkCreateInstance");
   if (!create_instance)
      fail("vkCreateInstance is unavailable");

   VkApplicationInfo application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "box64-vulkan-smoke",
      .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
      .pEngineName = "none",
      .engineVersion = VK_MAKE_VERSION(1, 0, 0),
      .apiVersion = VK_API_VERSION_1_0,
   };
   VkInstanceCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = create_instance(&create_info, NULL, &instance);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "VULKAN_SMOKE_FAIL: vkCreateInstance returned %d\n",
              result);
      return 1;
   }

   PFN_vkEnumeratePhysicalDevices enumerate_physical_devices =
      (PFN_vkEnumeratePhysicalDevices)get_instance_proc_addr(
         instance, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceProperties get_physical_device_properties =
      (PFN_vkGetPhysicalDeviceProperties)get_instance_proc_addr(
         instance, "vkGetPhysicalDeviceProperties");
   PFN_vkDestroyInstance destroy_instance =
      (PFN_vkDestroyInstance)get_instance_proc_addr(instance,
                                                    "vkDestroyInstance");
   if (!enumerate_physical_devices || !get_physical_device_properties ||
       !destroy_instance)
      fail("required instance functions are unavailable");

   uint32_t device_count = 0;
   result = enumerate_physical_devices(instance, &device_count, NULL);
   if (result != VK_SUCCESS || device_count == 0)
      fail("no Vulkan physical devices were enumerated");

   VkPhysicalDevice *devices = calloc(device_count, sizeof(*devices));
   if (!devices)
      fail("physical-device allocation failed");
   result = enumerate_physical_devices(instance, &device_count, devices);
   if (result != VK_SUCCESS)
      fail("physical-device enumeration failed");

   printf("VULKAN_LOADER=%u.%u.%u DEVICES=%u\n",
          VK_API_VERSION_MAJOR(loader_version),
          VK_API_VERSION_MINOR(loader_version),
          VK_API_VERSION_PATCH(loader_version), device_count);
   for (uint32_t index = 0; index < device_count; ++index) {
      VkPhysicalDeviceProperties properties;
      get_physical_device_properties(devices[index], &properties);
      printf("GPU[%u]=%s API=%u.%u.%u DRIVER=%u VENDOR=0x%04x DEVICE=0x%04x\n",
             index, properties.deviceName,
             VK_API_VERSION_MAJOR(properties.apiVersion),
             VK_API_VERSION_MINOR(properties.apiVersion),
             VK_API_VERSION_PATCH(properties.apiVersion),
             properties.driverVersion, properties.vendorID,
             properties.deviceID);
   }

   free(devices);
   destroy_instance(instance, NULL);
   dlclose(library);
   puts("VULKAN_SMOKE_OK");
   return 0;
}
