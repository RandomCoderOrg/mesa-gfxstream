#define _GNU_SOURCE
#define VK_USE_PLATFORM_XCB_KHR

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>

struct delayed_fence {
    VkInstance instance;
    VkDevice device;
    VkQueue queue;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkEvent event;
    VkFence fence;
    int sync_fd;
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
};

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
}

static xcb_screen_t *get_screen(xcb_connection_t *connection, int number)
{
    xcb_screen_iterator_t iterator =
        xcb_setup_roots_iterator(xcb_get_setup(connection));
    while (iterator.rem && number-- > 0)
        xcb_screen_next(&iterator);
    return iterator.data;
}

static int select_queue_family(VkPhysicalDevice physical)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *properties = calloc(count, sizeof(*properties));
    if (!properties)
        return -1;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, properties);
    int selected = -1;
    for (uint32_t index = 0; index < count; ++index) {
        if (properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            selected = (int)index;
            break;
        }
    }
    free(properties);
    return selected;
}

static void destroy_delayed_fence(struct delayed_fence *context)
{
    if (context->sync_fd >= 0)
        close(context->sync_fd);
    if (context->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(context->device);
        if (context->fence != VK_NULL_HANDLE)
            vkDestroyFence(context->device, context->fence, NULL);
        if (context->event != VK_NULL_HANDLE)
            vkDestroyEvent(context->device, context->event, NULL);
        if (context->command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(context->device, context->command_pool, NULL);
        vkDestroyDevice(context->device, NULL);
    }
    if (context->instance != VK_NULL_HANDLE)
        vkDestroyInstance(context->instance, NULL);
}

static int create_delayed_fence(struct delayed_fence *context)
{
    memset(context, 0, sizeof(*context));
    context->sync_fd = -1;

    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "udroid-x11-present-sync-file-wait",
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo instance_create = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    if (vkCreateInstance(&instance_create, NULL, &context->instance) != VK_SUCCESS)
        return 0;

    uint32_t physical_count = 0;
    if (vkEnumeratePhysicalDevices(context->instance, &physical_count, NULL) != VK_SUCCESS ||
        physical_count == 0)
        return 0;
    VkPhysicalDevice *physical_devices = calloc(physical_count, sizeof(*physical_devices));
    if (!physical_devices)
        return 0;
    VkResult result = vkEnumeratePhysicalDevices(context->instance, &physical_count,
                                                 physical_devices);
    VkPhysicalDevice physical = result == VK_SUCCESS ? physical_devices[0] : VK_NULL_HANDLE;
    free(physical_devices);
    if (physical == VK_NULL_HANDLE)
        return 0;

    VkPhysicalDeviceProperties physical_properties;
    vkGetPhysicalDeviceProperties(physical, &physical_properties);
    snprintf(context->device_name, sizeof(context->device_name), "%s",
             physical_properties.deviceName);

    int queue_family = select_queue_family(physical);
    if (queue_family < 0)
        return 0;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = (uint32_t)queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *extensions[] = {
        VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
    };
    VkDeviceCreateInfo device_create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = extensions,
    };
    if (vkCreateDevice(physical, &device_create, NULL, &context->device) != VK_SUCCESS)
        return 0;
    vkGetDeviceQueue(context->device, (uint32_t)queue_family, 0, &context->queue);

    VkCommandPoolCreateInfo pool_create = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = (uint32_t)queue_family,
    };
    if (vkCreateCommandPool(context->device, &pool_create, NULL,
                            &context->command_pool) != VK_SUCCESS)
        return 0;
    VkCommandBufferAllocateInfo command_allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = context->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(context->device, &command_allocate,
                                 &context->command_buffer) != VK_SUCCESS)
        return 0;
    VkEventCreateInfo event_create = {
        .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
    };
    if (vkCreateEvent(context->device, &event_create, NULL, &context->event) != VK_SUCCESS)
        return 0;

    VkCommandBufferBeginInfo command_begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(context->command_buffer, &command_begin) != VK_SUCCESS)
        return 0;
    vkCmdWaitEvents(context->command_buffer, 1, &context->event,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, NULL, 0, NULL, 0, NULL);
    if (vkEndCommandBuffer(context->command_buffer) != VK_SUCCESS)
        return 0;

    VkExportFenceCreateInfo export_create = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkFenceCreateInfo fence_create = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = &export_create,
    };
    if (vkCreateFence(context->device, &fence_create, NULL, &context->fence) != VK_SUCCESS)
        return 0;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &context->command_buffer,
    };
    if (vkQueueSubmit(context->queue, 1, &submit, context->fence) != VK_SUCCESS)
        return 0;

    PFN_vkGetFenceFdKHR get_fence_fd =
        (PFN_vkGetFenceFdKHR)vkGetDeviceProcAddr(context->device, "vkGetFenceFdKHR");
    if (!get_fence_fd)
        return 0;
    VkFenceGetFdInfoKHR get_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
        .fence = context->fence,
        .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    return get_fence_fd(context->device, &get_info, &context->sync_fd) == VK_SUCCESS &&
           context->sync_fd >= 0;
}

static int wait_for_complete(xcb_connection_t *connection,
                             xcb_special_event_t *special_event,
                             uint32_t serial, int timeout_ms)
{
    uint64_t deadline = monotonic_ms() + (uint64_t)timeout_ms;
    for (;;) {
        xcb_generic_event_t *event = xcb_poll_for_special_event(connection, special_event);
        if (event) {
            xcb_present_generic_event_t *present = (xcb_present_generic_event_t *)event;
            int matches = 0;
            if (present->evtype == XCB_PRESENT_COMPLETE_NOTIFY) {
                xcb_present_complete_notify_event_t *complete =
                    (xcb_present_complete_notify_event_t *)event;
                matches = complete->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP &&
                          complete->serial == serial;
            }
            free(event);
            if (matches)
                return 1;
            continue;
        }

        uint64_t now = monotonic_ms();
        if (now >= deadline)
            return 0;
        int remaining = (int)(deadline - now);
        struct pollfd descriptor = {
            .fd = xcb_get_file_descriptor(connection),
            .events = POLLIN,
        };
        int poll_result;
        do {
            poll_result = poll(&descriptor, 1, remaining);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result <= 0)
            return 0;
    }
}

static void timeout_handler(int signal_number)
{
    (void)signal_number;
    static const char message[] = "FAIL stage=global-timeout\n";
    ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
    _exit(124);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGALRM, timeout_handler);
    alarm(10);

    int screen_number = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
    xcb_screen_t *screen = get_screen(connection, screen_number);
    if (xcb_connection_has_error(connection) || !screen) {
        fprintf(stderr, "FAIL stage=xcb-connect\n");
        return 1;
    }

    xcb_window_t window = xcb_generate_id(connection);
    xcb_create_window(connection, screen->root_depth, window, screen->root,
                      0, 0, 64, 64, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, NULL);
    xcb_map_window(connection, window);
    xcb_pixmap_t pixmap = xcb_generate_id(connection);
    xcb_create_pixmap(connection, screen->root_depth, pixmap, window, 64, 64);
    xcb_gcontext_t gc = xcb_generate_id(connection);
    uint32_t foreground = screen->white_pixel;
    xcb_create_gc(connection, gc, pixmap, XCB_GC_FOREGROUND, &foreground);
    xcb_rectangle_t rectangle = { 0, 0, 64, 64 };
    xcb_poly_fill_rectangle(connection, pixmap, gc, 1, &rectangle);

    xcb_present_event_t event_id = xcb_generate_id(connection);
    xcb_special_event_t *special_event =
        xcb_register_for_special_xge(connection, &xcb_present_id, event_id, NULL);
    xcb_present_select_input(connection, event_id, window,
                             XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
    xcb_flush(connection);
    if (!special_event) {
        fprintf(stderr, "FAIL stage=present-events\n");
        return 1;
    }

    struct delayed_fence delayed;
    fprintf(stderr, "PROBE stage=create-blocked-vulkan-fence\n");
    if (!create_delayed_fence(&delayed)) {
        fprintf(stderr, "FAIL stage=create-blocked-vulkan-fence\n");
        destroy_delayed_fence(&delayed);
        return 1;
    }

    xcb_sync_fence_t wait_fence = xcb_generate_id(connection);
    xcb_void_cookie_t cookie = xcb_dri3_fence_from_fd_checked(
        connection, window, wait_fence, 0, delayed.sync_fd);
    delayed.sync_fd = -1; /* XCB owns the descriptor after request submission. */
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    if (error) {
        fprintf(stderr, "FAIL stage=import-sync-file error=%u\n", error->error_code);
        free(error);
        destroy_delayed_fence(&delayed);
        return 1;
    }

    const uint32_t serial = 0x5544524f;
    uint64_t submitted_at = monotonic_ms();
    cookie = xcb_present_pixmap_checked(connection, window, pixmap, serial,
                                        XCB_NONE, XCB_NONE, 0, 0, XCB_NONE,
                                        wait_fence, XCB_NONE,
                                        XCB_PRESENT_OPTION_NONE, 0, 0, 0, 0, NULL);
    error = xcb_request_check(connection, cookie);
    if (error) {
        fprintf(stderr, "FAIL stage=present-request error=%u\n", error->error_code);
        free(error);
        destroy_delayed_fence(&delayed);
        return 1;
    }

    int completed_early = wait_for_complete(connection, special_event, serial, 250);
    uint64_t released_at = monotonic_ms();
    if (completed_early) {
        fprintf(stderr, "FAIL stage=wait-fence-bypassed\n");
        destroy_delayed_fence(&delayed);
        return 1;
    }

    fprintf(stderr, "PROBE stage=release-vulkan-fence\n");
    if (vkSetEvent(delayed.device, delayed.event) != VK_SUCCESS) {
        fprintf(stderr, "FAIL stage=release-vulkan-fence\n");
        destroy_delayed_fence(&delayed);
        return 1;
    }
    int completed_after_release = wait_for_complete(connection, special_event,
                                                    serial, 3000);
    uint64_t completed_at = monotonic_ms();

    printf("{\"device\":\"%s\",\"completed_early\":%s,"
           "\"completed_after_release\":%s,\"blocked_ms\":%llu,"
           "\"release_to_complete_ms\":%llu}\n",
           delayed.device_name, completed_early ? "true" : "false",
           completed_after_release ? "true" : "false",
           (unsigned long long)(released_at - submitted_at),
           (unsigned long long)(completed_at - released_at));

    xcb_sync_destroy_fence(connection, wait_fence);
    xcb_unregister_for_special_event(connection, special_event);
    xcb_free_gc(connection, gc);
    xcb_free_pixmap(connection, pixmap);
    xcb_destroy_window(connection, window);
    xcb_flush(connection);
    xcb_disconnect(connection);
    destroy_delayed_fence(&delayed);
    alarm(0);
    return completed_after_release ? 0 : 1;
}
