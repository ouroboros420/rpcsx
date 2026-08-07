#include "stdafx.h"
#include "VulkanAPI.h"

#include "vkutils/device.h"

#define DECLARE_VK_FUNCTION_BODY
#include "VKProcTable.h"

namespace vk
{
	const render_device* get_current_renderer();

	void init()
	{
		auto pdev = get_current_renderer();

		// NOTE: Android loads the vulkan driver dynamically, so the loader entry point has to be resolved via the symbol cache.
		#define VK_FUNC(func) _##func = reinterpret_cast<PFN_##func>(VK_GET_SYMBOL(vkGetDeviceProcAddr)(*pdev, #func))
		#include "VKProcTable.h"
	}
}
