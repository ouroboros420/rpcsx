#pragma once

#include "swapchain_core.h"

#include <utility>
#include <vector>

namespace vk
{
#if defined(ANDROID)
	using swapchain_ANDROID = native_swapchain_base;
	using swapchain_NATIVE = swapchain_ANDROID;

	// Android permits only one *connected* VkSurfaceKHR per ANativeWindow, and the
	// Adreno/Turnip driver only releases the window's producer claim on an explicit
	// vkDestroySurfaceKHR - NOT when the surface is reaped as a child of
	// vkDestroyInstance. Across a savestate reload the renderer (and its VkInstance)
	// is fully destroyed and rebuilt, and opening the home menu reinitializes the
	// swapchain (swapchain_WSI::create -> a second make_WSI_surface), leaving the
	// previous surface to be reaped by vkDestroyInstance. That leftover keeps the
	// global ANativeWindow "in use", so the next session's vkCreateAndroidSurfaceKHR
	// fails with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR. Track every WSI surface so all of
	// them get an explicit vkDestroySurfaceKHR at teardown. Surface create/destroy is
	// serialized by the emulation lifecycle (one renderer at a time), so no lock.
	inline std::vector<std::pair<VkInstance, VkSurfaceKHR>> g_wsi_surfaces;

	static inline void track_WSI_surface(VkInstance vk_instance, VkSurfaceKHR surface)
	{
		if (surface != VK_NULL_HANDLE)
		{
			g_wsi_surfaces.emplace_back(vk_instance, surface);
		}
	}

	// Explicitly destroy every tracked surface that belongs to vk_instance. Must be
	// called only after all VkSwapchainKHR built on those surfaces are destroyed.
	static inline void destroy_WSI_surfaces(VkInstance vk_instance)
	{
		for (auto it = g_wsi_surfaces.begin(); it != g_wsi_surfaces.end();)
		{
			if (it->first == vk_instance)
			{
				if (it->second != VK_NULL_HANDLE)
				{
					VK_GET_SYMBOL(vkDestroySurfaceKHR)(it->first, it->second, nullptr);
				}
				it = g_wsi_surfaces.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	[[maybe_unused]] static VkSurfaceKHR make_WSI_surface(VkInstance vk_instance, display_handle_t window_handle, WSI_config* /*config*/)
	{
		VkSurfaceKHR result = VK_NULL_HANDLE;

		VkAndroidSurfaceCreateInfoKHR createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
		createInfo.window = std::get<ANativeWindow*>(window_handle);

		CHECK_RESULT(VK_GET_SYMBOL(vkCreateAndroidSurfaceKHR)(vk_instance, &createInfo, nullptr, &result));

		track_WSI_surface(vk_instance, result);
		return result;
	}
#endif
} // namespace vk
