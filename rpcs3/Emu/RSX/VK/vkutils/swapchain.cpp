#include "stdafx.h"
#include "swapchain.h"
#include "Emu/system_utils.hpp"

#ifdef ANDROID
#include <android/native_window.h>
#include <dlfcn.h>
#endif

namespace vk
{
	// Swapchain image RPCS3
	swapchain_image_RPCS3::swapchain_image_RPCS3(render_device& dev, const memory_type_mapping& memory_map, u32 width, u32 height)
		: image(dev, memory_map.device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, width, height, 1, 1, 1,
			  VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_TILING_OPTIMAL,
			  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, VMM_ALLOCATION_POOL_SWAPCHAIN)
	{
		m_width = width;
		m_height = height;
		current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

		m_dma_buffer = std::make_unique<buffer>(dev, m_width * m_height * 4, memory_map.host_visible_coherent,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, VMM_ALLOCATION_POOL_SWAPCHAIN);
	}

	void swapchain_image_RPCS3::do_dma_transfer(command_buffer& cmd)
	{
		VkBufferImageCopy copyRegion = {};
		copyRegion.bufferOffset = 0;
		copyRegion.bufferRowLength = m_width;
		copyRegion.bufferImageHeight = m_height;
		copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copyRegion.imageOffset = {};
		copyRegion.imageExtent = {m_width, m_height, 1};

		change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VK_GET_SYMBOL(vkCmdCopyImageToBuffer)(cmd, value, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_dma_buffer->value, 1, &copyRegion);
		change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	}

	u32 swapchain_image_RPCS3::get_required_memory_size() const
	{
		return m_width * m_height * 4;
	}

	void* swapchain_image_RPCS3::get_pixels()
	{
		return m_dma_buffer->map(0, VK_WHOLE_SIZE);
	}

	void swapchain_image_RPCS3::free_pixels()
	{
		m_dma_buffer->unmap();
	}

	// swapchain BASE
	swapchain_base::swapchain_base(physical_device& gpu, u32 present_queue, u32 graphics_queue, u32 transfer_queue, VkFormat format)
	{
		dev.create(gpu, graphics_queue, present_queue, transfer_queue);
		m_surface_format = format;
	}

	// NATIVE swapchain base
	VkResult native_swapchain_base::acquire_next_swapchain_image(VkSemaphore /*semaphore*/, u64 /*timeout*/, u32* result)
	{
		u32 index = 0;
		for (auto& p : swapchain_images)
		{
			if (!p.first)
			{
				p.first = true;
				*result = index;
				return VK_SUCCESS;
			}

			++index;
		}

		return VK_NOT_READY;
	}

	void native_swapchain_base::init_swapchain_images(render_device& dev, u32 preferred_count)
	{
		swapchain_images.resize(preferred_count);
		for (auto& img : swapchain_images)
		{
			img.second = std::make_unique<swapchain_image_RPCS3>(dev, dev.get_memory_mapping(), m_width, m_height);
			img.first = false;
		}
	}

	// WSI implementation
	void swapchain_WSI::init_swapchain_images(render_device& dev, u32 /*preferred_count*/)
	{
		u32 nb_swap_images = 0;
		_vkGetSwapchainImagesKHR(dev, m_vk_swapchain, &nb_swap_images, nullptr);

		if (!nb_swap_images)
			fmt::throw_exception("Driver returned 0 images for swapchain");

		std::vector<VkImage> vk_images;
		vk_images.resize(nb_swap_images);
		_vkGetSwapchainImagesKHR(dev, m_vk_swapchain, &nb_swap_images, vk_images.data());

		swapchain_images.resize(nb_swap_images);
		for (u32 i = 0; i < nb_swap_images; ++i)
		{
			swapchain_images[i].value = vk_images[i];
		}
	}

	swapchain_WSI::swapchain_WSI(vk::physical_device& gpu, u32 present_queue, u32 graphics_queue, u32 transfer_queue, VkFormat format, VkSurfaceKHR surface, VkColorSpaceKHR color_space, bool force_wm_reporting_off, display_handle_t window)
		: WSI_swapchain_base(gpu, present_queue, graphics_queue, transfer_queue, format)
	{
		_vkCreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(VK_GET_SYMBOL(vkGetDeviceProcAddr)(dev, "vkCreateSwapchainKHR"));
		_vkDestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(VK_GET_SYMBOL(vkGetDeviceProcAddr)(dev, "vkDestroySwapchainKHR"));
		_vkGetSwapchainImagesKHR = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(VK_GET_SYMBOL(vkGetDeviceProcAddr)(dev, "vkGetSwapchainImagesKHR"));
		_vkAcquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(VK_GET_SYMBOL(vkGetDeviceProcAddr)(dev, "vkAcquireNextImageKHR"));
		_vkQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(VK_GET_SYMBOL(vkGetDeviceProcAddr)(dev, "vkQueuePresentKHR"));

		m_surface = surface;
		m_color_space = color_space;
		// Keep the native window so present-time hints (ANativeWindow_setFrameRate on Android) can
		// reach it; the inherited window_handle is otherwise left default-null and the hint no-ops.
		window_handle = window;

		if (!force_wm_reporting_off)
		{
			switch (gpu.get_driver_vendor())
			{
			case driver_vendor::AMD:
			case driver_vendor::INTEL:
			case driver_vendor::RADV:
			case driver_vendor::MVK:
				break;
			case driver_vendor::ANV:
			case driver_vendor::NVIDIA:
				m_wm_reports_flag = true;
				break;
			default:
				break;
			}
		}
	}

	void swapchain_WSI::create(display_handle_t& handle)
	{
#ifdef ANDROID
		if (!dev)
		{
			return;
		}

		if (m_vk_swapchain)
		{
			_vkDestroySwapchainKHR(dev, m_vk_swapchain, nullptr);
			m_vk_swapchain = nullptr;
		}

		swapchain_images.clear();

		// The old swapchain was just destroyed above, so the previous surface(s)
		// for this instance no longer back any swapchain. Destroy them now instead
		// of letting them accumulate on the ANativeWindow until instance teardown:
		// repeated home-menu reinits would otherwise pile up live surfaces on the
		// one window and can re-trigger NATIVE_WINDOW_IN_USE within a session.
		destroy_WSI_surfaces(dev.gpu());

		WSI_config config{};
		m_surface = make_WSI_surface(dev.gpu(), handle, &config);
#endif
	}

	void swapchain_WSI::destroy(bool)
	{
		if (VkDevice pdev = dev)
		{
			if (m_vk_swapchain)
			{
				_vkDestroySwapchainKHR(pdev, m_vk_swapchain, nullptr);
			}

			dev.destroy();
		}
	}

	std::pair<VkSurfaceCapabilitiesKHR, bool> swapchain_WSI::init_surface_capabilities()
	{
#ifdef _WIN32
		if (g_cfg.video.vk.exclusive_fullscreen_mode != vk_exclusive_fs_mode::unspecified && dev.get_surface_capabilities_2_support())
		{
			HMONITOR hmonitor = MonitorFromWindow(window_handle, MONITOR_DEFAULTTOPRIMARY);
			if (hmonitor)
			{
				VkSurfaceCapabilities2KHR pSurfaceCapabilities = {};
				pSurfaceCapabilities.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;

				VkPhysicalDeviceSurfaceInfo2KHR pSurfaceInfo = {};
				pSurfaceInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
				pSurfaceInfo.surface = m_surface;

				VkSurfaceCapabilitiesFullScreenExclusiveEXT full_screen_exclusive_capabilities = {};
				VkSurfaceFullScreenExclusiveWin32InfoEXT full_screen_exclusive_win32_info = {};
				full_screen_exclusive_capabilities.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT;

				pSurfaceCapabilities.pNext = &full_screen_exclusive_capabilities;

				full_screen_exclusive_win32_info.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT;
				full_screen_exclusive_win32_info.hmonitor = hmonitor;

				pSurfaceInfo.pNext = &full_screen_exclusive_win32_info;

				auto getPhysicalDeviceSurfaceCapabilities2KHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
					VK_GET_SYMBOL(vkGetInstanceProcAddr)(dev.gpu(), "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
				ensure(getPhysicalDeviceSurfaceCapabilities2KHR);
				CHECK_RESULT(VK_GET_SYMBOL(getPhysicalDeviceSurfaceCapabilities2KHR)(dev.gpu(), &pSurfaceInfo, &pSurfaceCapabilities));

				return {pSurfaceCapabilities.surfaceCapabilities, !!full_screen_exclusive_capabilities.fullScreenExclusiveSupported};
			}
			else
			{
				rsx_log.warning("Swapchain: failed to get monitor for the window");
			}
		}
#endif
		VkSurfaceCapabilitiesKHR surface_descriptors = {};
		auto result = VK_GET_SYMBOL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(dev.gpu(), m_surface, &surface_descriptors);
#ifdef ANDROID
		if (result != VK_ERROR_SURFACE_LOST_KHR)
		{
			CHECK_RESULT(result);
		}
#else
		CHECK_RESULT(result);
#endif
		return {surface_descriptors, false};
	}

	bool swapchain_WSI::init()
	{
		if (dev.get_present_queue() == VK_NULL_HANDLE)
		{
			rsx_log.error("Cannot create WSI swapchain without a present queue");
			return false;
		}

		VkSwapchainKHR old_swapchain = m_vk_swapchain;
		vk::physical_device& gpu = const_cast<vk::physical_device&>(dev.gpu());

		auto [surface_descriptors, should_specify_exclusive_full_screen_mode] = init_surface_capabilities();

		if (surface_descriptors.maxImageExtent.width < m_width ||
			surface_descriptors.maxImageExtent.height < m_height)
		{
			rsx_log.error("Swapchain: Swapchain creation failed because dimensions cannot fit. Max = %d, %d, Requested = %d, %d",
				surface_descriptors.maxImageExtent.width, surface_descriptors.maxImageExtent.height, m_width, m_height);

			return false;
		}

		if (surface_descriptors.currentExtent.width != umax)
		{
			if (surface_descriptors.currentExtent.width == 0 || surface_descriptors.currentExtent.height == 0)
			{
				rsx_log.warning("Swapchain: Current surface extent is a null region. Is the window minimized?");
				return false;
			}

			m_width = surface_descriptors.currentExtent.width;
			m_height = surface_descriptors.currentExtent.height;
		}

		u32 nb_available_modes = 0;
		{
			const VkResult present_mode_result = VK_GET_SYMBOL(vkGetPhysicalDeviceSurfacePresentModesKHR)(gpu, m_surface, &nb_available_modes, nullptr);
#ifdef ANDROID
			// A surface bounce (app backgrounded during the boot/compile splash) makes this query
			// return SURFACE_LOST; treat it as recoverable (mirrors the capabilities guard above) -
			// bail so the per-frame reinitialize_swapchain() path can recreate the surface. 40b4b5f60682.
			if (present_mode_result == VK_ERROR_SURFACE_LOST_KHR)
			{
				rsx_log.warning("Swapchain: surface lost while querying present mode count; will recreate.");
				return false;
			}
#endif
			CHECK_RESULT(present_mode_result);
		}

		std::vector<VkPresentModeKHR> present_modes(nb_available_modes);
		{
			const VkResult present_mode_result = VK_GET_SYMBOL(vkGetPhysicalDeviceSurfacePresentModesKHR)(gpu, m_surface, &nb_available_modes, present_modes.data());
#ifdef ANDROID
			if (present_mode_result == VK_ERROR_SURFACE_LOST_KHR)
			{
				rsx_log.warning("Swapchain: surface lost while querying present modes; will recreate.");
				return false;
			}
#endif
			CHECK_RESULT(present_mode_result);
		}

		VkPresentModeKHR swapchain_present_mode = VK_PRESENT_MODE_FIFO_KHR;
		std::vector<VkPresentModeKHR> preferred_modes;

		if (rpcs3::utils::get_power_save_mode())
		{
			// Battery-saver overrides the saved vsync config and caps the GPU to the
			// display refresh (101810385680). It prefers FIFO_RELAXED: a frame that
			// finishes slightly late tears instead of slipping a whole refresh
			// interval, which smooths the 30fps-on-60/90/120Hz judder felt as
			// "stutter here and there". Falls back to plain FIFO if the driver does
			// not support relaxed.
			preferred_modes = {VK_PRESENT_MODE_FIFO_RELAXED_KHR};
		}
		else
		{
			switch (g_cfg.video.vsync)
			{
			case vsync_mode::off:
				preferred_modes = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR};
				break;
			case vsync_mode::adaptive:
				preferred_modes = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR };
				break;
			case vsync_mode::full:
			default:
				// FIFO is guaranteed to be supported, no need to go through a preference chain
				preferred_modes = {};
				break;
			}
		}

		bool mode_found = false;
		for (VkPresentModeKHR preferred_mode : preferred_modes)
		{
			// Search for this mode in supported modes
			for (VkPresentModeKHR mode : present_modes)
			{
				if (mode == preferred_mode)
				{
					swapchain_present_mode = mode;
					mode_found = true;
					break;
				}
			}

			if (mode_found)
				break;
		}

		rsx_log.notice("Swapchain: present mode %d in use.", static_cast<int>(swapchain_present_mode));

		u32 nb_swap_images = surface_descriptors.minImageCount + 1;
		if (surface_descriptors.maxImageCount > 0)
		{
			// Try to negotiate for a triple buffer setup
			// In cases where the front-buffer isnt available for present, its better to have a spare surface
			nb_swap_images = std::max(surface_descriptors.minImageCount + 2u, 3u);

			if (nb_swap_images > surface_descriptors.maxImageCount)
			{
				// Application must settle for fewer images than desired:
				nb_swap_images = surface_descriptors.maxImageCount;
			}
		}

		VkSurfaceTransformFlagBitsKHR pre_transform = surface_descriptors.currentTransform;
		if (surface_descriptors.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
			pre_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;

		VkSwapchainCreateInfoKHR swap_info = {};
		swap_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		swap_info.surface = m_surface;
		swap_info.minImageCount = nb_swap_images;
		swap_info.imageFormat = m_surface_format;
		swap_info.imageColorSpace = m_color_space;

		swap_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		swap_info.preTransform = pre_transform;
		swap_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		swap_info.imageArrayLayers = 1;
		swap_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		swap_info.presentMode = swapchain_present_mode;
		swap_info.oldSwapchain = old_swapchain;
		swap_info.clipped = true;

		swap_info.imageExtent.width = std::max(m_width, surface_descriptors.minImageExtent.width);
		swap_info.imageExtent.height = std::max(m_height, surface_descriptors.minImageExtent.height);

#ifdef _WIN32
		VkSurfaceFullScreenExclusiveInfoEXT full_screen_exclusive_info = {};
		if (should_specify_exclusive_full_screen_mode)
		{
			vk_exclusive_fs_mode fs_mode = g_cfg.video.vk.exclusive_fullscreen_mode;
			ensure(fs_mode == vk_exclusive_fs_mode::enable || fs_mode == vk_exclusive_fs_mode::disable);

			full_screen_exclusive_info.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
			full_screen_exclusive_info.fullScreenExclusive =
				fs_mode == vk_exclusive_fs_mode::enable ? VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT : VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;

			swap_info.pNext = &full_screen_exclusive_info;
		}

		rsx_log.notice("Swapchain: requesting full screen exclusive mode %d.", static_cast<int>(full_screen_exclusive_info.fullScreenExclusive));
#endif

#ifdef ANDROID
		// Harden the previously-unchecked create on Android: a surface lost here (backgrounded
		// during the boot/compile splash) is recoverable. Bail BEFORE destroying old_swapchain or
		// calling init_swapchain_images (which would throw on zero images); keep old_swapchain as
		// the live handle so create()'s unconditional destroy reclaims it on the recovery pass. Any
		// other create failure stays fatal. 40b4b5f60682.
		{
			const VkResult create_result = _vkCreateSwapchainKHR(dev, &swap_info, nullptr, &m_vk_swapchain);
			if (create_result == VK_ERROR_SURFACE_LOST_KHR)
			{
				rsx_log.warning("Swapchain: surface lost during vkCreateSwapchainKHR; will recreate.");
				m_vk_swapchain = old_swapchain;
				return false;
			}
			CHECK_RESULT(create_result);
		}
#else
		_vkCreateSwapchainKHR(dev, &swap_info, nullptr, &m_vk_swapchain);
#endif

		if (old_swapchain)
		{
			if (!swapchain_images.empty())
			{
				swapchain_images.clear();
			}

			_vkDestroySwapchainKHR(dev, old_swapchain, nullptr);
		}

		init_swapchain_images(dev);
		return true;
	}

	VkResult swapchain_WSI::present(VkSemaphore semaphore, u32 image)
	{
#ifdef ANDROID
		// Tell the panel the content cadence so a 90/120Hz display can align its refresh to a
		// clean multiple for steady 30/60fps games (less judder, lower power). Advisory only; the
		// hint is re-pushed only when the snapped fps changes, so it costs nothing per frame.
		// ANativeWindow_setFrameRate* is API 30+ and minSdk is 29, so resolve at runtime (null ->
		// pre-30 device, skip) rather than a compile-time availability guard, which the strict core
		// build rejects.
		//
		// These symbols are exported by libnativewindow.so, NOT libandroid.so (which only provides
		// ANativeWindow_fromSurface/_acquire - the ANativeWindow_* symbols the core links).
		// libnativewindow is not in this .so's DT_NEEDED group, so a plain dlsym(RTLD_DEFAULT, ...)
		// cannot see them on Android's namespaced linker (which is why the hint was once silently
		// inert). dlopen the library by name to bring it into scope; the handle is kept for process
		// lifetime (libnativewindow stays resident, never dlclose'd).
		//
		// Prefer the API-31 ...WithChangeStrategy variant: it lets us pass ONLY_IF_SEAMLESS so the
		// panel switches refresh rate ONLY when it can do so without a visible (flickering) mode
		// change. Fall back to the plain API-30 setter where the strategy variant is absent.
		using sfr_strat_fn = int32_t (*)(ANativeWindow *, float, int8_t, int8_t);
		using sfr_plain_fn = int32_t (*)(ANativeWindow *, float, int8_t);
		struct sfr_fns { sfr_strat_fn strat; sfr_plain_fn plain; };
		static const sfr_fns s_sfr = []() -> sfr_fns
		{
			void* const lib = dlopen("libnativewindow.so", RTLD_NOW);
			if (!lib)
			{
				return { nullptr, nullptr };
			}
			return {
				reinterpret_cast<sfr_strat_fn>(dlsym(lib, "ANativeWindow_setFrameRateWithChangeStrategy")),
				reinterpret_cast<sfr_plain_fn>(dlsym(lib, "ANativeWindow_setFrameRate")),
			};
		}();
		const bool sfr_available = s_sfr.strat || s_sfr.plain;

		// ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE (1): emulated PS3 output is inherently
		// fixed-cadence content, so the system should target a clean multiple of the hinted rate.
		// ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS (0): never trigger a flickering mode switch.
		constexpr int8_t sfr_compat_fixed_source = 1;
		constexpr int8_t sfr_change_only_if_seamless = 0;

		// One-shot confirmation: with the dlopen fix this stays silent and the success notice below
		// fires instead; if it ever logs, the symbols still failed to resolve on device.
		[[maybe_unused]] static const bool s_sfr_diag = []
		{
			if (!s_sfr.strat && !s_sfr.plain)
				rsx_log.error("Android: ANativeWindow_setFrameRate* unavailable (libnativewindow dlopen/dlsym failed)");
			return true;
		}();
		if (sfr_available)
		{
			const u64 period_ns = rpcs3::utils::get_frame_period_ns();
			if (period_ns != 0)
			{
				const float fps = 1.0e9f / static_cast<float>(period_ns);
				if (fps > 1.0f && fps < 1000.0f)
				{
					// Quantize the noisy per-flip rate to the nearest real PS3 cadence, then push the
					// hint only after that cadence has held for a sustained window. get_frame_period_ns
					// is the RAW per-flip wall interval (min ~16ms / max ~700ms+ within one second on
					// jittery scenes), so a plain snap-and-push fired ANativeWindow_setFrameRate almost
					// every frame, making SurfaceFlinger renegotiate the panel refresh rate continuously
					// = beat-frequency judder under FIFO, the opposite of the hint's intent. Quantize +
					// debounce hands the compositor ONE steady target per scene cadence and filters out
					// loading/hitch transients (e.g. the spurious 89fps seen during a 30fps boot).
					auto quantize_cadence = [](float f) -> float
					{
						static constexpr float cadences[] = { 24.f, 25.f, 30.f, 50.f, 60.f };
						for (const float c : cadences)
						{
							const float d = f > c ? f - c : c - f;
							if (d <= c * 0.12f) // within 12% snaps to the cadence
								return c;
						}
						return static_cast<float>(static_cast<long>(f + 0.5f));
					};
					const float snapped = quantize_cadence(fps);

					// Debounce ~0.75s of flips (rate-agnostic frame count) so a transient outlier
					// resets but never reaches the panel; only a sustained cadence is pushed.
					constexpr u32 stable_frames_required = 24;
					if (snapped == m_pending_frame_rate_hint)
					{
						if (m_frame_rate_hint_stable_count < stable_frames_required)
							++m_frame_rate_hint_stable_count;
					}
					else
					{
						m_pending_frame_rate_hint = snapped;
						m_frame_rate_hint_stable_count = 1;
					}

					if (m_frame_rate_hint_stable_count >= stable_frames_required && snapped != m_last_frame_rate_hint)
					{
						if (auto awnd = std::get_if<ANativeWindow *>(&window_handle); awnd && *awnd)
						{
							if (s_sfr.strat)
								s_sfr.strat(*awnd, snapped, sfr_compat_fixed_source, sfr_change_only_if_seamless);
							else
								s_sfr.plain(*awnd, snapped, sfr_compat_fixed_source);
							m_last_frame_rate_hint = snapped;
							static bool s_logged_once = false;
							if (!s_logged_once)
							{
								rsx_log.notice("Android: ANativeWindow_setFrameRate hint active (%.0f fps, %s)", static_cast<double>(snapped), s_sfr.strat ? "seamless" : "legacy");
								s_logged_once = true;
							}
						}
					}
				}
			}
		}
#endif

		VkPresentInfoKHR present = {};
		present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		present.pNext = nullptr;
		present.swapchainCount = 1;
		present.pSwapchains = &m_vk_swapchain;
		present.pImageIndices = &image;

		if (semaphore != VK_NULL_HANDLE)
		{
			present.waitSemaphoreCount = 1;
			present.pWaitSemaphores = &semaphore;
		}

		return _vkQueuePresentKHR(dev.get_present_queue(), &present);
	}
} // namespace vk
