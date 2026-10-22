#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "graphics/host_gpu/graphicContext.h"
#include "common/threads.h"
#include "kernel/pthread.h"

#include <chrono>
#include <cstdio>

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick, const char* why) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	// Long waits are the signature of a wedged GPU or an unsubmitted tick.
	// Wait in 1s slices so a stuck tick is logged (every 5s) instead of going
	// silent; semantics stay an indefinite wait.
	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::GpuWaits,
	                                         Common::DebugCounters::Counter::GpuWaitNs);
	vk::Result result = vk::Result::eTimeout;
	uint64_t slices = 0;
	while (result == vk::Result::eTimeout) {
		result = m_graphics.device.waitSemaphores(&wait_info, 1000000000ull);
		if (result != vk::Result::eSuccess && result != vk::Result::eTimeout) {
			break;
		}
		if (result == vk::Result::eTimeout && (++slices % 5) == 0) {
			char thread_name[64] = "?";
			if (auto self = Libs::LibKernel::PthreadSelfOrNull(); self != nullptr) {
				if (Libs::LibKernel::PthreadGetname(self, thread_name) != 0) {
					std::snprintf(thread_name, sizeof(thread_name), "guest-%d",
					            Common::Thread::GetThreadIdUnique());
				}
			} else {
				std::snprintf(thread_name, sizeof(thread_name), "host-%d",
				            Common::Thread::GetThreadIdUnique());
			}
			std::printf("GpuFenceWait: still waiting tick=%llu (%llus) thread=%s why=%s\n",
			            static_cast<unsigned long long>(tick),
			            static_cast<unsigned long long>(slices), thread_name, why);
			std::fflush(stdout);
		}
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Refresh();
}

} // namespace Libs::Graphics
