#pragma once
#include "VulkanAPI.h"
#include <deque>
#include <vector>

namespace vk
{
	class command_buffer;
	class query_pool;
	class render_device;

	class query_pool_manager
	{
		struct query_slot_info
		{
			query_pool* pool;
			bool any_passed;
			bool active;
			bool ready;
			u32 data;
		};

		class query_pool_ref
		{
			std::unique_ptr<query_pool> m_object;
			query_pool_manager* m_pool_man;

		public:
			query_pool_ref(query_pool_manager* pool_man, std::unique_ptr<query_pool>& pool)
				: m_object(std::move(pool)), m_pool_man(pool_man)
			{
			}

			~query_pool_ref();
		};

		std::vector<std::unique_ptr<query_pool>> m_consumed_pools;
		std::deque<std::unique_ptr<query_pool>> m_query_pool_cache;
		std::unique_ptr<query_pool> m_current_query_pool;
		std::deque<u32> m_available_slots;
		u32 m_pool_lifetime_counter = 0;

		VkQueryType query_type = VK_QUERY_TYPE_OCCLUSION;
		VkQueryResultFlags result_flags = VK_QUERY_RESULT_PARTIAL_BIT;
		VkQueryControlFlags control_flags = 0;

		vk::render_device* owner = nullptr;
		std::vector<query_slot_info> query_slot_status;

		// A tile-based renderer cannot produce a query result before its tiling
		// pass resolves, so spinning on one burns a core for the whole pass.
		// Sleep between polls instead. Decided once, at construction.
		bool tile_based_renderer = false;

		bool poke_query(query_slot_info& query, u32 index, VkQueryResultFlags flags);
		void allocate_new_pool(vk::command_buffer& cmd);
		void reallocate_pool(vk::command_buffer& cmd);
		void run_pool_cleanup();

	public:
		query_pool_manager(vk::render_device& dev, VkQueryType type, u32 num_entries);
		~query_pool_manager();

		void set_control_flags(VkQueryControlFlags control_flags, VkQueryResultFlags result_flags);

		void begin_query(vk::command_buffer& cmd, u32 index);
		void end_query(vk::command_buffer& cmd, u32 index);

		bool check_query_status(u32 index);
		u32 get_query_result(u32 index);
		void get_query_result_indirect(vk::command_buffer& cmd, u32 index, u32 count, VkBuffer dst, VkDeviceSize dst_offset);

		// Batched readback: GPU-copy each query result in `indices` order to `dst` (4-byte word at
		// offset = position in the list), coalescing contiguous index runs that share a pool into single
		// vkCmdCopyQueryPoolResults calls. Pool-aware so a numeric run spanning a pool reallocation
		// boundary is never copied from the wrong pool. Ends any open renderpass first (TBDR requirement).
		void copy_query_results(vk::command_buffer& cmd, const std::vector<u32>& indices, VkBuffer dst);

		// Prime a slot's cached result from a host-read value (no GPU call) so a subsequent
		// get_query_result returns it with no WAIT_BIT. Only valid for an allocated slot whose result was
		// already waited (e.g. via the WAIT_BIT copy above).
		void prime_query_result(u32 index, u32 value);

		u32 allocate_query(vk::command_buffer& cmd);
		void free_query(vk::command_buffer& /*cmd*/, u32 index);

		void on_query_pool_released(std::unique_ptr<vk::query_pool>& pool);

		template<typename T>
			requires std::ranges::range<T> && std::same_as<std::ranges::range_value_t<T>, u32> // List of u32
		void free_queries(vk::command_buffer& cmd, T& list)
		{
			for (const auto index : list)
			{
				free_query(cmd, index);
			}
		}
	};
}; // namespace vk
