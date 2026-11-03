#include "stdafx.h"
#include "VKGSRender.h"
#include "../Common/BufferUtils.h"
#include "../rsx_methods.h"
#include "vkutils/buffer_object.h"
#include "VKDMA.h"

#include <bit>
#include <span>

#include "Emu/RSX/NV47/HW/context_accessors.define.h"

namespace vk
{
	std::pair<VkPrimitiveTopology, bool> get_appropriate_topology(rsx::primitive_type mode)
	{
		switch (mode)
		{
		case rsx::primitive_type::lines:
			return { VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false };
		case rsx::primitive_type::line_loop:
			return { VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, true };
		case rsx::primitive_type::line_strip:
			return { VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, false };
		case rsx::primitive_type::points:
			return { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, false };
		case rsx::primitive_type::triangles:
			return { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false };
		case rsx::primitive_type::triangle_strip:
		case rsx::primitive_type::quad_strip:
			return { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, false };
		case rsx::primitive_type::triangle_fan:
#ifndef __APPLE__
			return { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, false };
#endif
		case rsx::primitive_type::quads:
		case rsx::primitive_type::polygon:
			return { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true };
		default:
			fmt::throw_exception("Unsupported primitive topology 0x%x", static_cast<u8>(mode));
		}
	}

	bool is_primitive_native(rsx::primitive_type mode)
	{
		return !get_appropriate_topology(mode).second;
	}

	VkIndexType get_index_type(rsx::index_array_type type)
	{
		switch (type)
		{
		case rsx::index_array_type::u32:
			return VK_INDEX_TYPE_UINT32;
		case rsx::index_array_type::u16:
			return VK_INDEX_TYPE_UINT16;
		}
		fmt::throw_exception("Invalid index array type (%u)", static_cast<u8>(type));
	}
}

namespace
{
	std::tuple<u32, std::tuple<VkDeviceSize, VkIndexType>> generate_emulating_index_buffer(
		const rsx::draw_clause& clause, u32 vertex_count,
		vk::data_heap& m_index_buffer_ring_info)
	{
		u32 index_count = get_index_count(clause.primitive, vertex_count);
		u32 upload_size = index_count * sizeof(u32);

		VkDeviceSize offset_in_index_buffer = m_index_buffer_ring_info.alloc<256>(upload_size);
		void* buf = m_index_buffer_ring_info.map(offset_in_index_buffer, upload_size);

		g_fxo->get<rsx::dma_manager>().emulate_as_indexed(buf, clause.primitive, vertex_count);

		m_index_buffer_ring_info.unmap();
		return std::make_tuple(
			index_count, std::make_tuple(offset_in_index_buffer, VK_INDEX_TYPE_UINT32));
	}

	struct vertex_input_state
	{
		VkPrimitiveTopology native_primitive_type;
		bool index_rebase;
		u32 min_index;
		u32 max_index;
		u32 vertex_draw_count;
		u32 vertex_index_offset;
		std::optional<std::tuple<VkDeviceSize, VkIndexType>> index_info;
	};

	struct draw_command_visitor
	{
		draw_command_visitor(rsx::context* ctx, vk::data_heap& index_buffer_ring_info, rsx::vertex_input_layout& layout)
			: m_ctx(ctx)
			, m_index_buffer_ring_info(index_buffer_ring_info)
			, m_vertex_layout(layout)
		{
		}

		vertex_input_state operator()(const rsx::draw_array_command& /*command*/)
		{
			const auto [prims, primitives_emulated] = vk::get_appropriate_topology(REGS(m_ctx)->current_draw_clause.primitive);
			const u32 vertex_count = REGS(m_ctx)->current_draw_clause.get_elements_count();
			const u32 min_index = REGS(m_ctx)->current_draw_clause.min_index();
			const u32 max_index = (min_index + vertex_count) - 1;

			if (primitives_emulated)
			{
				u32 index_count;
				std::optional<std::tuple<VkDeviceSize, VkIndexType>> index_info;

				std::tie(index_count, index_info) =
					generate_emulating_index_buffer(REGS(m_ctx)->current_draw_clause,
						vertex_count, m_index_buffer_ring_info);

				return{ prims, false, min_index, max_index, index_count, 0, index_info };
			}

			return{ prims, false, min_index, max_index, vertex_count, 0, {} };
		}

		vertex_input_state operator()(const rsx::draw_indexed_array_command& command)
		{
			const auto& input_assembly = m_ctx->draw_state.input_assembly;
			const auto primitive = REGS(m_ctx)->current_draw_clause.primitive;
			const auto [prims, primitives_emulated] = vk::get_appropriate_topology(primitive);
			const bool emulate_restart = input_assembly.restart_index_enabled && vk::emulate_primitive_restart(primitive);

			rsx::index_array_type index_type = REGS(m_ctx)->current_draw_clause.is_immediate_draw ?
				rsx::index_array_type::u32 :
				input_assembly.index_type;

			u32 type_size = get_index_type_size(index_type);

			u32 index_count = REGS(m_ctx)->current_draw_clause.get_elements_count();
			if (primitives_emulated)
				index_count = get_index_count(primitive, index_count);
			u32 upload_size = index_count * type_size;

			if (emulate_restart) upload_size *= 2;

			VkDeviceSize offset_in_index_buffer = m_index_buffer_ring_info.alloc<64>(upload_size);
			void* buf = m_index_buffer_ring_info.map(offset_in_index_buffer, upload_size);

			std::span<std::byte> dst;
			stx::single_ptr<std::byte[]> tmp;
			if (emulate_restart)
			{
				tmp = stx::make_single<std::byte[], false, 64>(upload_size);
				dst = std::span<std::byte>(tmp.get(), upload_size);
			}
			else
			{
				dst = std::span<std::byte>(static_cast<std::byte*>(buf), upload_size);
			}

			/**
			* Upload index (and expands it if primitive type is not natively supported).
			*/
			u32 min_index, max_index;
			std::tie(min_index, max_index, index_count) = write_index_array_data_to_buffer(
				dst,
				command.raw_index_buffer, index_type,
				REGS(m_ctx)->current_draw_clause.primitive,
				input_assembly.restart_index_enabled,
				input_assembly.restart_index,
				[](auto prim) { return !vk::is_primitive_native(prim); });

			if (min_index > max_index ||
				(min_index == max_index && primitive != rsx::primitive_type::points))
			{
				// empty set, do not draw
				m_index_buffer_ring_info.unmap();
				return{ prims, false, 0, 0, 0, 0, {} };
			}

			if (emulate_restart)
			{
				if (index_type == rsx::index_array_type::u16)
				{
					index_count = rsx::remove_restart_index(static_cast<u16*>(buf), reinterpret_cast<u16*>(tmp.get()), index_count, u16{umax});
				}
				else
				{
					index_count = rsx::remove_restart_index(static_cast<u32*>(buf), reinterpret_cast<u32*>(tmp.get()), index_count, u32{umax});
				}
			}

			m_index_buffer_ring_info.unmap();

			std::optional<std::tuple<VkDeviceSize, VkIndexType>> index_info =
				std::make_tuple(offset_in_index_buffer, vk::get_index_type(index_type));

			const auto index_offset = REGS(m_ctx)->vertex_data_base_index();
			return {prims, true, min_index, max_index, index_count, index_offset, index_info};
		}

		vertex_input_state operator()(const rsx::draw_inlined_array& /*command*/)
		{
			auto &draw_clause = REGS(m_ctx)->current_draw_clause;
			const auto [prims, primitives_emulated] = vk::get_appropriate_topology(draw_clause.primitive);

			const auto stream_length = REGS(m_ctx)->current_draw_clause.inline_vertex_array.size();
			const u32 vertex_count = u32(stream_length * sizeof(u32)) / m_vertex_layout.interleaved_blocks[0]->attribute_stride;

			if (!primitives_emulated)
			{
				return{ prims, false, 0, vertex_count - 1, vertex_count, 0, {} };
			}

			u32 index_count;
			std::optional<std::tuple<VkDeviceSize, VkIndexType>> index_info;
			std::tie(index_count, index_info) = generate_emulating_index_buffer(draw_clause, vertex_count, m_index_buffer_ring_info);
			return{ prims, false, 0, vertex_count - 1, index_count, 0, index_info };
		}

	private:
		rsx::context* m_ctx;
		vk::data_heap& m_index_buffer_ring_info;
		rsx::vertex_input_layout& m_vertex_layout;
	};
}

vk::vertex_upload_info VKGSRender::upload_vertex_data()
{
	draw_command_visitor visitor(m_ctx, m_index_buffer_ring_info, m_vertex_layout);
	auto result = std::visit(visitor, RSX(m_ctx)->draw_processor()->get_draw_command(*REGS(m_ctx)));

	const u32 vertex_count = (result.max_index - result.min_index) + 1;
	u32 vertex_base = result.min_index;
	u32 index_base = 0;

	if (result.index_rebase)
	{
		vertex_base = rsx::get_index_from_base(vertex_base, REGS(m_ctx)->vertex_data_base_index());
		index_base = result.min_index;
	}

	//Do actual vertex upload
	auto required = RSX(m_ctx)->draw_processor()->calculate_memory_requirements(m_vertex_layout, vertex_base, vertex_count);
	u32 persistent_range_base = -1, volatile_range_base = -1;
	usz persistent_offset = -1, volatile_offset = -1;

	// Zero-copy vertex streams. With passthrough DMA the guest range a single interleaved block
	// covers is bound to the shader in place: the attribute fetch already reads guest layout
	// (stride, offset, byte swap) from a byte texel buffer, so the copy into the attribute ring
	// and the vertex-cache bookkeeping around it are the only things this skips. Draws with
	// several blocks are bound as one view over the span that covers them all, each block at its
	// own offset in it (fill_vertex_layout_state takes the per-block table). The span is capped:
	// blocks far apart in memory would make the DMA layer import everything between them, so
	// those draws keep the copy.
	bool persistent_aliased = false;
	m_persistent_alias_active = false;
	m_aliased_volatile_mask = 0;

	// Blocks too far from the bound cluster: copied after the volatile data instead.
	struct far_block_t { u32 start, length, volatile_offset; };
	std::array<far_block_t, 16> far_blocks{};
	u32 far_block_count = 0;
	const u32 volatile_data_size = required.second;

	if (required.first > 0 && m_config.supports_passthrough_dma &&
		m_vertex_layout.interleaved_blocks.size() <= m_aliased_block_offsets.size() &&
		REGS(m_ctx)->current_draw_clause.command != rsx::draw_command::inlined_array)
	{
		constexpr u32 max_span = 8 * 1024 * 1024;
		std::array<u32, 16> block_start{};
		std::array<u32, 16> block_length{};
		u32 span_begin = umax, span_end = 0, total = 0;
		bool usable = true;

		for (usz i = 0; i < m_vertex_layout.interleaved_blocks.size() && usable; i++)
		{
			const auto block = m_vertex_layout.interleaved_blocks[i];
			const auto range = block->calculate_required_range(vertex_base, vertex_count);
			const u32 length = range.second * block->attribute_stride;
			const u32 start = block->real_offset_address + range.first * block->attribute_stride;

			usable = length && start + length > start;
			block_start[i] = start;
			block_length[i] = length;
			total += length;
			span_begin = std::min(span_begin, start);
			span_end = std::max(span_end, start + length);
		}

		// Too far apart (typically some arrays in main memory and some in RSX local memory, GBs
		// apart): bind the cluster around the largest block, and copy the others.
		u16 far_mask = 0;
		u32 far_bytes = 0;

		if (usable && total == required.first && span_end - span_begin > max_span)
		{
			usz anchor = 0;

			for (usz i = 1; i < m_vertex_layout.interleaved_blocks.size(); i++)
			{
				if (block_length[i] > block_length[anchor]) anchor = i;
			}

			span_begin = block_start[anchor];
			span_end = block_start[anchor] + block_length[anchor];

			for (usz i = 0; i < m_vertex_layout.interleaved_blocks.size(); i++)
			{
				if (i == anchor) continue;

				const u32 begin = std::min(span_begin, block_start[i]);
				const u32 end = std::max(span_end, block_start[i] + block_length[i]);

				if (end - begin <= max_span)
				{
					span_begin = begin;
					span_end = end;
				}
				else
				{
					far_mask |= static_cast<u16>(1u << i);
					far_blocks[far_block_count++] = { block_start[i], block_length[i], utils::align(volatile_data_size, 16u) + far_bytes };
					far_bytes += utils::align(block_length[i], 16u);
				}
			}
		}

		u32 span_base = 0;

		if (usable && total == required.first && span_end - span_begin <= max_span &&
			bind_aliased_vertex_range(span_begin, span_end - span_begin, span_base))
		{
			for (usz i = 0; i < m_vertex_layout.interleaved_blocks.size(); i++)
			{
				m_aliased_block_offsets[i] = (far_mask >> i) & 1
					? far_blocks[std::popcount(static_cast<u16>(far_mask & ((1u << i) - 1)))].volatile_offset
					: span_base + (block_start[i] - span_begin);
			}

			if (far_mask)
			{
				m_aliased_volatile_mask = far_mask;
				required.second = utils::align(volatile_data_size, 16u) + far_bytes;
			}
			else
			{
				far_block_count = 0;
			}

			persistent_range_base = span_base;
			persistent_aliased = true;
			m_persistent_alias_active = true;
		}
	}

	if (required.first > 0 && !persistent_aliased)
	{
		//Check if cacheable
		//Only data in the 'persistent' block may be cached
		//TODO: make vertex cache keep local data beyond frame boundaries and hook notify command
		bool in_cache = false;
		bool to_store = false;
		u32  storage_address = -1;

		m_ctx->stats.vertex_cache_request_count++;

		if (m_vertex_layout.interleaved_blocks.size() == 1 &&
			REGS(m_ctx)->current_draw_clause.command != rsx::draw_command::inlined_array)
		{
			const auto data_offset = (vertex_base * m_vertex_layout.interleaved_blocks[0]->attribute_stride);
			storage_address = m_vertex_layout.interleaved_blocks[0]->real_offset_address + data_offset;

			if (auto cached = res()->vertex_cache->find_vertex_range(storage_address, required.first))
			{
				ensure(cached->local_address == storage_address);

				in_cache = true;
				persistent_range_base = cached->offset_in_heap;
			}
			else
			{
				to_store = true;
			}
		}

		if (!in_cache)
		{
			m_ctx->stats.vertex_cache_miss_count++;

			persistent_offset = static_cast<u32>(m_attrib_ring_info.alloc<256>(required.first));
			persistent_range_base = static_cast<u32>(persistent_offset);

			if (to_store)
			{
				//store ref in vertex cache
				res()->vertex_cache->store_range(storage_address, required.first, static_cast<u32>(persistent_offset));
			}
		}
	}

	if (required.second > 0)
	{
		volatile_offset = static_cast<u32>(m_attrib_ring_info.alloc<256>(required.second));
		volatile_range_base = static_cast<u32>(volatile_offset);
	}

	//Write all the data once if possible
	if (required.first && required.second && volatile_offset > persistent_offset)
	{
		//Do this once for both to save time on map/unmap cycles
		const usz block_end = (volatile_offset + required.second);
		const usz block_size = block_end - persistent_offset;
		const usz volatile_offset_in_block = volatile_offset - persistent_offset;

		void *block_mapping = m_attrib_ring_info.map(persistent_offset, block_size);
		RSX(m_ctx)->draw_processor()->write_vertex_data_to_memory(m_vertex_layout, vertex_base, vertex_count, block_mapping, static_cast<char*>(block_mapping) + volatile_offset_in_block);
		m_attrib_ring_info.unmap();
	}
	else
	{
		if (required.first > 0 && persistent_offset != umax)
		{
			void *persistent_mapping = m_attrib_ring_info.map(persistent_offset, required.first);
			RSX(m_ctx)->draw_processor()->write_vertex_data_to_memory(m_vertex_layout, vertex_base, vertex_count, persistent_mapping, nullptr);
			m_attrib_ring_info.unmap();
		}

		if (required.second > 0)
		{
			void *volatile_mapping = m_attrib_ring_info.map(volatile_offset, required.second);

			if (volatile_data_size)
			{
				RSX(m_ctx)->draw_processor()->write_vertex_data_to_memory(m_vertex_layout, vertex_base, vertex_count, nullptr, volatile_mapping);
			}

			for (u32 i = 0; i < far_block_count && m_persistent_alias_active; i++)
			{
				std::memcpy(static_cast<char*>(volatile_mapping) + far_blocks[i].volatile_offset, vm::_ptr<char>(far_blocks[i].start), far_blocks[i].length);
			}

			m_attrib_ring_info.unmap();
		}
	}

	if (vk::test_status_interrupt(vk::heap_changed))
	{
		// Check for validity
		if (m_persistent_attribute_storage &&
			m_persistent_attribute_storage->info.buffer != m_attrib_ring_info.heap->value)
		{
			vk::get_resource_manager()->dispose(m_persistent_attribute_storage);
		}

		if (m_volatile_attribute_storage &&
			m_volatile_attribute_storage->info.buffer != m_attrib_ring_info.heap->value)
		{
			vk::get_resource_manager()->dispose(m_volatile_attribute_storage);
		}

		m_vertex_env_buffer_info = { *m_vertex_env_ring_info.heap, 0, VK_WHOLE_SIZE };
		m_vertex_constants_buffer_info = { *m_transform_constants_ring_info.heap, 0, VK_WHOLE_SIZE };
		m_fragment_env_buffer_info = { *m_fragment_env_ring_info.heap, 0, VK_WHOLE_SIZE };
		m_fragment_texture_params_buffer_info = { *m_fragment_texture_params_ring_info.heap, 0, VK_WHOLE_SIZE };
		m_raster_env_buffer_info = { *m_raster_env_ring_info.heap, 0, VK_WHOLE_SIZE };
		m_vertex_layout_stream_info = { *m_vertex_layout_ring_info.heap, 0, VK_WHOLE_SIZE };
		m_fragment_constants_buffer_info = { *m_fragment_constants_ring_info.heap, 0, VK_WHOLE_SIZE };

		vk::clear_status_interrupt(vk::heap_changed);
	}

	if (persistent_range_base != umax && !persistent_aliased)
	{
		if (!m_persistent_attribute_storage || !m_persistent_attribute_storage->in_range(persistent_range_base, required.first, persistent_range_base))
		{
			ensure(m_texbuffer_view_size >= required.first); // "Incompatible driver (MacOS?)"
			vk::get_resource_manager()->dispose(m_persistent_attribute_storage);

			//View 64M blocks at a time (different drivers will only allow a fixed viewable heap size, 64M should be safe)
			const usz view_size = (persistent_range_base + m_texbuffer_view_size) > m_attrib_ring_info.size() ? m_attrib_ring_info.size() - persistent_range_base : m_texbuffer_view_size;
			m_persistent_attribute_storage = std::make_unique<vk::buffer_view>(*m_device, m_attrib_ring_info.heap->value, VK_FORMAT_R8_UINT, persistent_range_base, view_size);
			persistent_range_base = 0;
		}
	}

	if (volatile_range_base != umax)
	{
		if (!m_volatile_attribute_storage || !m_volatile_attribute_storage->in_range(volatile_range_base, required.second, volatile_range_base))
		{
			ensure(m_texbuffer_view_size >= required.second); // "Incompatible driver (MacOS?)"
			vk::get_resource_manager()->dispose(m_volatile_attribute_storage);

			const usz view_size = (volatile_range_base + m_texbuffer_view_size) > m_attrib_ring_info.size() ? m_attrib_ring_info.size() - volatile_range_base : m_texbuffer_view_size;
			m_volatile_attribute_storage = std::make_unique<vk::buffer_view>(*m_device, m_attrib_ring_info.heap->value, VK_FORMAT_R8_UINT, volatile_range_base, view_size);
			volatile_range_base = 0;
		}
	}

	return{ result.native_primitive_type,                 // Primitive
			result.vertex_draw_count,                     // Vertex count
			vertex_count,                                 // Allocated vertex count
			vertex_base,                                  // First vertex in stream
			index_base,                                   // Index of vertex at data location 0
			result.vertex_index_offset,                   // Index offset
			persistent_range_base, volatile_range_base,   // Binding range
			result.index_info };                          // Index buffer info
}

// Binds [guest_address, +length) through the DMA block that covers it, as a byte texel buffer
// view, and returns the offset of guest_address inside that view. The view starts at the block
// offset rounded down to the device's texel buffer alignment; it is reused while the draw's range
// stays inside it and the block's buffer is unchanged (map_dma can replace a block with a larger
// one; the old buffer is then retired through the resource manager once the GPU is done).
// load_dma is a no-op for an imported block and a copy for a staged fallback, so a range the
// kernel would not import is still served correctly, at today's cost.
bool VKGSRender::bind_aliased_vertex_range(u32 guest_address, u32 length, u32& persistent_range_base)
{
	const auto [block_offset, dma_buffer] = vk::map_dma(guest_address, length);

	if (!dma_buffer)
	{
		return false;
	}

	vk::load_dma(guest_address, length);

	u32 offset_in_view = 0;

	if (!m_aliased_attribute_storage ||
		m_aliased_attribute_storage->info.buffer != dma_buffer->value ||
		!m_aliased_attribute_storage->in_range(block_offset, length, offset_in_view))
	{
		const u32 view_base = block_offset & ~(m_texel_buffer_offset_align - 1);
		const u64 view_size = std::min<u64>(m_texbuffer_view_size, dma_buffer->size() - view_base);

		if (view_size < (block_offset - view_base) + length)
		{
			return false;
		}

		vk::get_resource_manager()->dispose(m_aliased_attribute_storage);
		m_aliased_attribute_storage = std::make_unique<vk::buffer_view>(*m_device, dma_buffer->value, VK_FORMAT_R8_UINT, view_base, view_size);
		offset_in_view = block_offset - view_base;

		static bool s_announced = false;

		if (!std::exchange(s_announced, true))
		{
			rsx_log.notice("Vertex streams are bound in place over guest memory (passthrough DMA).");
		}
	}

	persistent_range_base = offset_in_view;
	return true;
}
