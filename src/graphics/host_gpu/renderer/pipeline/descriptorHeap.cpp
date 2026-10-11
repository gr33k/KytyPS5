#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <atomic>
#include <cstdio>

namespace Libs::Graphics {
namespace {

constexpr uint32_t   DescriptorHeapCount = 1024;
constexpr std::array DescriptorPoolSizes = {
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageBuffer, 32768},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampledImage, 32768},
    // Single layouts can bind hundreds of storage images/samplers in total
    // across many bindings (e.g. RE Engine streaming kernels); keep every
    // pool well above any one layout's summed demand so a fresh pool can
    // always satisfy a single set.
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageImage, 8192},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampler, 8192},
};

} // namespace

DescriptorHeap::DescriptorHeap(GraphicContext& graphics, MasterSemaphore& master_semaphore)
    : m_graphics(graphics), m_master_semaphore(master_semaphore) {
	CreateDescriptorPool();
}

DescriptorHeap::~DescriptorHeap() {
	m_graphics.device.destroyDescriptorPool(m_current_pool, nullptr);
	for (const auto& [pool, tick]: m_pending_pools) {
		m_master_semaphore.Wait(tick);
		m_graphics.device.destroyDescriptorPool(pool, nullptr);
	}
}

vk::DescriptorSet DescriptorHeap::Commit(vk::DescriptorSetLayout layout) {
	KYTY_PROFILER_FUNCTION();
	std::lock_guard lock(m_mutex);
	EXIT_IF(layout == nullptr);

	auto& batch = m_sets[layout];
	if (batch.size != 0) {
		return batch.sets[--batch.size];
	}
	if (Allocate(layout, batch)) {
		return batch.sets[--batch.size];
	}

	m_pending_pools.emplace_back(m_current_pool, m_master_semaphore.CurrentTick());
	{
		auto& [pool, tick] = m_pending_pools.front();
		// Prefer reusing a rested pool over creating a new one: pools created
		// while the GPU is backed up can report fragmented on first use
		// (observed on amdvlk). A short hitch beats a wedged pool.
		if (!m_master_semaphore.IsFree(tick)) {
			m_master_semaphore.Wait(tick);
		}
		m_current_pool = pool;
		m_pending_pools.pop_front();
		EXIT_IF(m_graphics.device.resetDescriptorPool(m_current_pool, {}) != vk::Result::eSuccess);
	}

	const uint32_t failed_alloc = batch.allocation;
	m_sets.clear();
	auto& fresh_batch = m_sets[layout];
	if (!Allocate(layout, fresh_batch)) {
		// A reset pool can retain unusable state on some drivers, failing
		// allocation despite adequate capacity. Fall back to a brand-new pool
		// before giving up.
		static std::atomic_uint pool_retry_count {0};
		if (pool_retry_count.fetch_add(1, std::memory_order_relaxed) < 8) {
			std::printf("DescriptorHeap: fresh-pool allocate failed, retrying with new pool\n");
			std::fflush(stdout);
		}
		m_pending_pools.emplace_back(m_current_pool, m_master_semaphore.CurrentTick());
		CreateDescriptorPool();
		m_sets.clear();
		auto& retry_batch = m_sets[layout];
		bool retry_ok = Allocate(layout, retry_batch);
		if (!retry_ok) {
			// Last resort: the driver reports even brand-new pools as fragmented
			// (observed on amdvlk after hundreds of thousands of submits).
			// Drain the GPU, tear down every pool, and start over. A
			// multi-second hitch beats killing the game.
			std::printf("DescriptorHeap: draining GPU and rebuilding all pools\n");
			std::fflush(stdout);
			const uint64_t last_tick = m_master_semaphore.CurrentTick();
			if (last_tick > 0) {
				m_master_semaphore.Wait(last_tick - 1);
			}
			for (const auto& [pool, tick]: m_pending_pools) {
				m_graphics.device.destroyDescriptorPool(pool, nullptr);
			}
			m_pending_pools.clear();
			m_graphics.device.destroyDescriptorPool(m_current_pool, nullptr);
			CreateDescriptorPool();
			m_sets.clear();
			auto& rebuilt_batch = m_sets[layout];
			retry_ok            = Allocate(layout, rebuilt_batch);
			if (retry_ok) {
				return rebuilt_batch.sets[--rebuilt_batch.size];
			}
		}
		if (!retry_ok) {
			// Give up on this draw/dispatch instead of killing the game: the
			// caller skips it (logged, rate-limited). Observed on amdvlk where
			// fresh pools can report fragmented after heavy turnover.
			static std::atomic_uint skip_log_count {0};
			if (skip_log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
				std::printf("DescriptorHeap: no set for layout=%p, skipping draw/dispatch\n",
				            static_cast<void*>(layout));
				std::fflush(stdout);
			}
			return nullptr;
		}
		return retry_batch.sets[--retry_batch.size];
	}
	return fresh_batch.sets[--fresh_batch.size];
}

bool DescriptorHeap::Allocate(vk::DescriptorSetLayout layout, Batch& batch) {
	std::array<vk::DescriptorSetLayout, DescriptorSetBatch> layouts;
	layouts.fill(layout);

	vk::DescriptorSetAllocateInfo allocate {};
	allocate.descriptorPool = m_current_pool;
	allocate.pSetLayouts    = layouts.data();

	for (;;) {
		allocate.descriptorSetCount = batch.allocation;
		const auto result = m_graphics.device.allocateDescriptorSets(&allocate, batch.sets.data());
		if (result == vk::Result::eSuccess) {
			batch.size = batch.allocation;
			return true;
		}
		EXIT_IF(result != vk::Result::eErrorOutOfPoolMemory &&
		        result != vk::Result::eErrorFragmentedPool);
		if (batch.allocation == 1) {
			std::printf("DescriptorHeap: alloc failed layout=%p count=%u result=%d pool=%p\n",
			            static_cast<void*>(layout), allocate.descriptorSetCount,
			            static_cast<int>(result), static_cast<void*>(m_current_pool));
			std::fflush(stdout);
			return false;
		}
		batch.allocation /= 2;
	}
}

void DescriptorHeap::CreateDescriptorPool() {
	vk::DescriptorPoolCreateInfo create {};
	create.maxSets       = DescriptorHeapCount;
	create.poolSizeCount = static_cast<uint32_t>(DescriptorPoolSizes.size());
	create.pPoolSizes    = DescriptorPoolSizes.data();
	EXIT_IF(m_graphics.device.createDescriptorPool(&create, nullptr, &m_current_pool) !=
	        vk::Result::eSuccess);
}

} // namespace Libs::Graphics
