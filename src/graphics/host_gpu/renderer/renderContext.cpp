#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	return true;
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		if (m_command_scheduler.Active()) {
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void RenderContext::PrepareBda() {
	std::shared_lock lock(m_mapped_ranges_mutex);
	m_mapped_ranges.ForEach([this](uint64_t start, uint64_t end) {
		m_buffer_cache.SynchronizeBuffersInRange(start, end - start);
	});
	m_fault_process_pending = true;
}

void RenderContext::RunGarbageCollector() {
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}


namespace {
constexpr uint64_t OcclusionReadyBit = 1ull << 63u;
constexpr uint32_t OcclusionDbCount   = 16u;
constexpr uint8_t  OcclusionHysteresisFrames = 2u;

void WriteOcclusionResult(uint64_t address, bool ready, uint64_t value) {
	if (address == 0) {
		return;
	}
	auto* results = reinterpret_cast<volatile uint64_t*>(address);
	const uint64_t result = ready ? (OcclusionReadyBit | value) : 0;
	for (uint32_t db = 0; db < OcclusionDbCount; db++) {
		results[db * 2u] = result;
	}
}
} // namespace

void RenderContext::BeginOcclusionEvent(const CommandBuffer& command, uint64_t event_address) {
	if (m_occlusion_active) {
		// Nested/overlapping occlusion events are not expected by the guest. Do not let a malformed
		// stream affect subsequent rendering; finish the previous interval conservatively.
		WriteOcclusionResult(m_occlusion_begin_address, true, 1);
		m_occlusion_active = false;
		m_occlusion_pending_query.reset();
		m_occlusion_current_query.reset();
		m_occlusion_queries.clear();
	}

	bool predicted_visible = true;
	{
		std::lock_guard lock(m_occlusion_mutex);
		const auto it = m_occlusion_history.find(event_address);
		if (it != m_occlusion_history.end()) {
			predicted_visible =
				it->second.occluded_frames < OcclusionHysteresisFrames;
		}
	}

	// Never make the PM4 stream wait for a future Vulkan result. The guest sees a completed,
	// conservative result immediately; the current GPU query updates the prediction for the next
	// use of this result slot.
	WriteOcclusionResult(event_address, true, 0);

	m_occlusion_active           = true;
	m_occlusion_begin_address   = event_address;
	m_occlusion_predicted_visible = predicted_visible;
	m_occlusion_fallback_visible = false;
	m_occlusion_queries.clear();
	m_occlusion_pending_query.reset();
	m_occlusion_current_query.reset();

	if (command.IsRendering()) {
		PrepareOcclusionRendering(command);
		BeginOcclusionRendering(command);
	}
}

void RenderContext::PrepareOcclusionRendering(const CommandBuffer& command) {
	if (!m_occlusion_active || m_occlusion_pending_query.has_value() ||
	    m_occlusion_current_query.has_value()) {
		return;
	}

	std::lock_guard lock(m_occlusion_mutex);

	if (m_occlusion_query_pool == nullptr) {
		vk::QueryPoolCreateInfo info {};
		info.queryType = vk::QueryType::eOcclusion;
		info.queryCount = OcclusionQueryCount;
		auto [result, pool] = m_graphics.device.createQueryPool(info);
		EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
		m_occlusion_query_pool = pool;
	}

	uint32_t query = 0;
	if (!m_free_occlusion_queries.empty()) {
		query = m_free_occlusion_queries.back();
		m_free_occlusion_queries.pop_back();
	} else if (m_next_occlusion_query < OcclusionQueryCount) {
		query = m_next_occlusion_query++;
	} else {
		m_occlusion_fallback_visible = true;
		static std::once_flag warning_once;
		std::call_once(warning_once, [] {
			std::printf("Warning: Vulkan occlusion query pool exhausted; using conservative visible result.\\n");
		});
		return;
	}

	m_graphics.device.resetQueryPool(m_occlusion_query_pool, query, 1);
	m_occlusion_pending_query = query;
}

void RenderContext::BeginOcclusionRendering(const CommandBuffer& command) {
	if (!m_occlusion_active || !m_occlusion_pending_query.has_value() ||
	    m_occlusion_current_query.has_value()) {
		return;
	}
	const uint32_t query = *m_occlusion_pending_query;
	command.Handle().beginQuery(m_occlusion_query_pool, query, {});
	m_occlusion_current_query = query;
	m_occlusion_pending_query.reset();
}

void RenderContext::EndOcclusionRendering(const CommandBuffer& command) {
	if (!m_occlusion_current_query.has_value()) {
		return;
	}
	const uint32_t query = *m_occlusion_current_query;
	command.Handle().endQuery(m_occlusion_query_pool, query);
	m_occlusion_queries.push_back(query);
	m_occlusion_current_query.reset();
}

void RenderContext::EndOcclusionEvent(const CommandBuffer& command, uint64_t event_address) {
	if (!m_occlusion_active) {
		// A stray end event is kept conservative and, importantly, does not touch an unrelated
		// memory location asynchronously.
		WriteOcclusionResult(event_address, true, 1);
		return;
	}

	EndOcclusionRendering(command);

	const uint64_t begin_address = m_occlusion_begin_address;
	auto queries = std::move(m_occlusion_queries);
	m_occlusion_queries.clear();
	const bool fallback_visible = m_occlusion_fallback_visible;
	const bool predicted_visible = m_occlusion_predicted_visible;

	m_occlusion_active = false;
	m_occlusion_pending_query.reset();
	m_occlusion_current_query.reset();
	m_occlusion_fallback_visible = false;

	// Publish the temporally-filtered result now. The guest predication path therefore never waits
	// on the host query worker. The Vulkan result below is only used to update the next prediction.
	WriteOcclusionResult(event_address, true, predicted_visible ? 1 : 0);

	if (queries.empty() || m_occlusion_query_pool == nullptr) {
		return;
	}

	const auto pool = m_occlusion_query_pool;
	auto* const device = &m_graphics.device;

	m_command_scheduler.DeferPriorityOperation(
		[pool, device, begin_address, queries = std::move(queries), fallback_visible, this]() mutable {
			bool visible = false;
			bool query_failed = false;
			std::array<uint64_t, 1> result {};

			for (const uint32_t query : queries) {
				const auto status = device->getQueryPoolResults(
					pool, query, 1, sizeof(uint64_t), result.data(), sizeof(uint64_t),
					vk::QueryResultFlagBits::e64);
				if (status == vk::Result::eSuccess) {
					visible |= result[0] != 0;
				} else {
					query_failed = true;
				}

			std::lock_guard lock(m_occlusion_mutex);
			m_free_occlusion_queries.push_back(query);
			}

			// Any retrieval failure or exhausted-pool interval is treated as visible. Never train the
			// temporal predictor from an invalid/unknown result.
			visible |= fallback_visible || query_failed;

			std::lock_guard lock(m_occlusion_mutex);
			auto& history = m_occlusion_history[begin_address];
		if (visible) {
				history.occluded_frames = 0;
		} else {
				history.occluded_frames =
					static_cast<uint8_t>(std::min<uint32_t>(
						history.occluded_frames + 1u, OcclusionHysteresisFrames));
			}
			if (m_occlusion_history.size() > 4096u) {
				m_occlusion_history.clear();
			}
		});
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
