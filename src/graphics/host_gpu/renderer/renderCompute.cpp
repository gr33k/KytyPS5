#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "libs/controller.h"
#include "kernel/memory.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include <cstdio>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {
static bool FillSourcesDisjoint(std::span<const ShaderRecompiler::IR::DescriptorValue> sources,
                                 GuestRange destination, uint32_t output_buffer = UINT32_MAX) {
	for (uint32_t i = 0; i < sources.size(); ++i) {
		if (i == output_buffer) continue;
		const auto source = DecodeNativeDescriptor<ShaderBufferResource>(sources[i]);
		const auto bytes  = source.GetSize();
		if (source.Base48() < destination.End() && destination.address < source.Base48() + bytes)
			return false;
	}
	return true;
}

bool RenderExecutor::TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
                                                const CommandBuffer&          buffer) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		EXIT("compute runtime buffer count does not match shader metadata\n");
	}
	auto& cache = buffer.GetContext().GetTextureCache();
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		// A metadata resource that is also read is not proven to be a full overwrite. Execute it
		// conservatively instead of replacing the dispatch with a coarse full-surface clear.
		if ((!resource.written || resource.read) && cache.IsMeta(descriptor.Base48())) {
			return false;
		}
	}

	if (!program.info.has_bitwise_xor) {
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& resource = program.info.buffers[i];
			if (resource.written) {
				const auto descriptor =
				    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
				if (cache.ClearMeta(descriptor.Base48())) {
					return true;
				}
			}
		}
	}
	return false;
}

bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode,
                              ShaderBufferResource& resolved_descriptor, uint32_t& resolved_clear,
                              uint64_t& resolved_size) {
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	if (fill.kind != ShaderRecompiler::IR::UniformFillKind::Buffer) {
		return false;
	}
	const auto element_size = fill.words * sizeof(uint32_t);
	const auto descriptor =
	    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[fill.resource]);
	constexpr std::array formats {
	    Prospero::BufferFormat::k32UInt, Prospero::BufferFormat::k32_32UInt,
	    Prospero::BufferFormat::k32_32_32UInt, Prospero::BufferFormat::k32_32_32_32UInt};
	if (descriptor.Stride() != element_size || descriptor.Format() != formats[fill.words - 1] ||
	    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid() ||
	    descriptor.Base48() == 0) {
		return false;
	}
	if (input.threads_num[0] == 0 || input.threads_num[0] != fill.group_stride[0] ||
	    input.threads_num[1] != 1 || input.threads_num[2] != 1 || group_x == 0 || group_y != 1 ||
	    group_z != 1 || mode != (input.dispatch_thread_dimensions ? 0x61u : 0x41u)) {
		return false;
	}
	const uint64_t invocations = input.dispatch_thread_dimensions
	                                 ? group_x
	                                 : static_cast<uint64_t>(group_x) * input.threads_num[0];
	const auto     size        = descriptor.GetSize();
	if (invocations != descriptor.NumRecords() || size == 0 || size > UINT32_MAX ||
	    (input.dispatch_thread_dimensions &&
	     (group_x % input.threads_num[0] != 0 || input.dispatch_threads_num[0] != group_x ||
	      input.dispatch_threads_num[1] != 1 || input.dispatch_threads_num[2] != 1))) {
		return false;
	}
	if (!FillSourcesDisjoint(resources.buffers, {descriptor.Base48(), size}, fill.resource))
		return false;
	resolved_descriptor = descriptor;
	resolved_clear      = fill.value;
	resolved_size       = size;
	return true;
}

bool RenderExecutor::TryConsumeComputeImageClear(const ShaderComputeInputInfo& input,
                                                CommandBuffer& command, uint32_t group_x,
                                                uint32_t group_y, uint32_t group_z, uint32_t mode) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	auto&       cache     = command.GetContext().GetTextureCache();
	if (fill.kind == ShaderRecompiler::IR::UniformFillKind::Image) {
		if (mode != 0x41u || input.dispatch_thread_dimensions || fill.value > 255 ||
		    input.threads_num[2] != 1)
			return false;
		const auto  descriptor = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[0]);
		const auto& resource   = program.info.images[0];
		if (descriptor.IsNull() || descriptor.Format() != Prospero::BufferFormat::k8UInt ||
		    descriptor.Type() != Prospero::ImageType::kColor2DArray || descriptor.MetaCompress() ||
		    descriptor.WriteCompress() || descriptor.BaseLevel() > descriptor.LastLevel() ||
		    descriptor.BaseLevel() > descriptor.MaxMip() ||
		    descriptor.BaseArray5() > descriptor.Depth() || descriptor.DstSelX() != 4)
			return false;
		const std::array extents {
		    std::max(1u, (descriptor.Width5() + 1u) >> descriptor.BaseLevel()),
		    std::max(1u, (descriptor.Height5() + 1u) >> descriptor.BaseLevel()),
		    descriptor.Depth() - descriptor.BaseArray5() + 1u};
		const std::array groups {group_x, group_y, group_z};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const uint64_t threads = input.threads_num[axis];
			// Guest image writes outside the descriptor dimensions are discarded. Only the
			// final workgroup may extend beyond the selected image view.
			if (threads == 0 || threads != fill.group_stride[axis] ||
			    groups[axis] != (extents[axis] + threads - 1) / threads ||
			    groups[axis] * threads > UINT32_MAX) return false;
		}
		const auto  binding     = ResolveTexture(resource, resources.images[0]);
		const auto& destination = binding.desc.info.data;
		if (!FillSourcesDisjoint(resources.buffers, destination)) return false;
		std::scoped_lock lock {cache.m_lock};
		const auto&      image = cache.GetImage(binding.image_id);
		const auto&      view  = binding.desc.view_info;
		if (image.backing.format != vk::Format::eD32SfloatS8Uint || image.info.samples != 1 ||
		    image.info.stencil != destination || view.base_level >= image.backing.mip_levels ||
		    view.base_layer >= image.backing.layers || view.layer_count != extents[2] ||
		    view.layer_count > image.backing.layers - view.base_layer ||
		    std::max(1u, image.info.extent.width >> view.base_level) != extents[0] ||
		    std::max(1u, image.info.extent.height >> view.base_level) != extents[1]) return false;
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eStencil, view.base_level,
		                                       1, view.base_layer, view.layer_count};
		vk::ClearValue clear {};
		clear.depthStencil = vk::ClearDepthStencilValue {0.0f, fill.value};
		cache.ClearImage(command, binding.image_id, image.backing.format, range, clear);
		return true;
	}
	ShaderBufferResource descriptor;
	uint32_t             packed_clear = 0;
	uint64_t             size         = 0;
	if (!ResolveComputeBufferFill(input, group_x, group_y, group_z, mode, descriptor, packed_clear,
	                              size)) {
		return false;
	}
	if (!cache.ClearImageFromBuffer(command, descriptor.Base48(), size, packed_clear)) {
		return false;
	}
	static std::atomic<uint32_t> logged_clears {0};
	if (logged_clears.fetch_add(1, std::memory_order_relaxed) < 32) {
		LOGF("GraphicsRenderDispatchDirect: compute image clear shader=0x%016" PRIx64
		     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " value=0x%08" PRIx32 "\n",
		     input.stage.program->shader_hash, descriptor.Base48(), size, packed_clear);
	}
	return true;
}

static void BindSharedMemory(RenderContext& context, ShaderComputeInputInfo& input,
                             PreparedBindings& bindings, uint64_t indirect_args = 0) {
	if (input.stage.program->bindings.descriptor_counts[
	        static_cast<size_t>(ShaderRecompiler::IR::DescriptorBindingKind::SharedMemory)] == 0) {
		return;
	}
	auto& cache = context.GetBufferCache();
	if (indirect_args != 0) {
		cache.ReadMemory(indirect_args, sizeof(vk::DispatchIndirectCommand));
		std::memcpy(input.workgroup_counts, reinterpret_cast<const void*>(indirect_args),
		            sizeof(input.workgroup_counts));
	}
	// LDS has no contents to preserve between dispatches. The existing shader hazard
	// barriers also order other users of this GPU-only utility buffer.
	auto& storage = cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
	const auto limit = std::min<uint64_t>(storage.Size(),
	    context.GetGraphics().GetPhysicalDeviceProperties().limits.maxStorageBufferRange);
	uint64_t size = sizeof(uint32_t);
	if (std::ranges::find(input.workgroup_counts, 0u) == std::end(input.workgroup_counts)) {
		size = uint64_t {input.lds_size_dwords} * sizeof(uint32_t);
		EXIT_IF(size == 0 || size > limit);
		for (const auto count: input.workgroup_counts) {
			EXIT_IF(size > limit / count);
			size *= count;
		}
	}
	bindings.shared_memory = {storage.Handle(), 0, size};
}

namespace {
// RE Engine Maiden rescue: kernel 0xdd7d9b716cfe5aba is a straight-line
// streaming memcpy (verified in disassembly: two bounds-checked dword copies
// per thread, no loops) that hangs some GPUs on specific post-start inputs.
// Emulate it on the CPU so the game receives its data without executing it.
// Matches the disassembly exactly: index = workgroup.x * 512 + lane, second
// copy at +256, byte offsets from push dword 0 bits [0:8]/[8:16], OOB reads
// yield 0, OOB writes are dropped. Returns false when the invocation does
// not match the known shape (caller falls back to skipping).
bool CpuEmulateStreamingCopy(RenderContext& context, const ShaderComputeInputInfo& input_info,
                             const ShaderRecompiler::IR::CompiledShaderInfo& program,
                             PreparedBindings& bindings, uint32_t gx, uint32_t gy, uint32_t gz) {
	if (program.info.buffers.size() != 2 || gy != 1 || gz != 1) {
		return false;
	}
	if (input_info.threads_num[0] != 256 || input_info.threads_num[1] != 1 ||
	    input_info.threads_num[2] != 1) {
		return false;
	}
	int src_i = -1, dst_i = -1;
	for (uint32_t i = 0; i < 2; i++) {
		if (program.info.buffers[i].written) {
			if (dst_i >= 0) {
				return false;
			}
			dst_i = static_cast<int>(i);
		} else {
			if (src_i >= 0) {
				return false;
			}
			src_i = static_cast<int>(i);
		}
	}
	if (src_i < 0 || dst_i < 0) {
		return false;
	}
	const auto& src_info = program.info.buffers[src_i];
	const auto& dst_info = program.info.buffers[dst_i];
	if (src_info.descriptor_index == UINT32_MAX || dst_info.descriptor_index == UINT32_MAX ||
	    src_info.descriptor_index >= bindings.buffer_sources.size() ||
	    dst_info.descriptor_index >= bindings.buffer_sources.size()) {
		return false;
	}
	const auto& resources = *input_info.stage.resources;
	const auto  src_desc =
	    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[src_i]);
	const auto dst_desc =
	    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[dst_i]);
	if (src_desc.Stride() != 4 || dst_desc.Stride() != 4) {
		return false;
	}
	if (program.bindings.push_data_start_dword ==
	        ShaderRecompiler::IR::PushData::NoStart ||
	    program.bindings.push_data_start_dword >= bindings.shader_data.size()) {
		return false;
	}
	const uint32_t push_dword = bindings.shader_data[program.bindings.push_data_start_dword];
	const uint32_t off0       = push_dword & 0xFFu;
	const uint32_t off1       = (push_dword >> 8) & 0xFFu;
	{
		static std::atomic_uint shape_logged {0};
		if (shape_logged.fetch_add(1, std::memory_order_relaxed) < 2) {
			std::printf("Dd7dShape: bufs=%zu src_di=%u dst_di=%u nsrc=%zu push_start=%u ndw=%zu "
			            "gx=%u threads=%ux%ux%u\n",
			            program.info.buffers.size(), src_info.descriptor_index,
			            dst_info.descriptor_index, bindings.buffer_sources.size(),
			            program.bindings.push_data_start_dword, bindings.shader_data.size(),
			            gx, input_info.threads_num[0], input_info.threads_num[1],
			            input_info.threads_num[2]);
			std::fflush(stdout);
		}
	}
	const auto&    src_range  = bindings.buffer_sources[src_info.descriptor_index];
	const auto&    dst_range  = bindings.buffer_sources[dst_info.descriptor_index];
	if (src_range.address == 0 || dst_range.address == 0) {
		return false;
	}
	const uint64_t src_dwords = src_range.size / 4;
	const uint64_t dst_dwords = dst_range.size / 4;
	if (dst_dwords == 0) {
		return true;
	}
	uint64_t total = static_cast<uint64_t>(gx) * 512 + 256;
	if (total > dst_dwords) {
		total = dst_dwords;
	}
	// Throughput guard: huge streaming copies cost tens of milliseconds on
	// the CPU; emulate only up to 256K dwords and let larger ones skip (the
	// game polls small control ranges, bulk data follows later).
	if (total > (256ull << 10)) {
		return false;
	}
	if (total == 0 || total > (64ull << 20)) {
		return total == 0;
	}
	// Read the source range once; holes mean the data is not resident.
	std::vector<uint8_t> src_bytes;
	uint64_t             src_avail_dwords = 0;
	if (src_dwords > 0) {
		src_bytes.resize(static_cast<size_t>(src_range.size));
		if (!Libs::LibKernel::Memory::TryReadBufferBacking(src_range.address, src_bytes.data(),
		                                                    src_range.size)) {
			return false;
		}
		src_avail_dwords = src_dwords;
	}
	const auto*         src_words = reinterpret_cast<const uint32_t*>(src_bytes.data());
	std::vector<uint32_t> out(static_cast<size_t>(total));
	for (uint64_t k = 0; k < total; k++) {
		const uint64_t read_byte = k * 4 + off0;
		uint32_t       v         = 0;
		// Overflow guard mirrors the shader: (byte32 < off) yields an
		// index of 0xFFFFFFFF, which always fails the bounds check.
		if (read_byte <= 0xFFFFFFFFull) {
			const uint32_t b32 = static_cast<uint32_t>(read_byte);
			if (b32 >= off0) {
				const uint32_t w = b32 >> 2;
				if (w < src_avail_dwords) {
					v = src_words[w];
				}
			}
		}
		const uint64_t write_byte = k * 4 + off1;
		if (write_byte > 0xFFFFFFFFull) {
			continue;
		}
		const uint32_t d32 = static_cast<uint32_t>(write_byte);
		if (d32 < off1) {
			continue;
		}
		const uint32_t dw = d32 >> 2;
		if (dw < dst_dwords && dw < total) {
			out[dw] = v;
		}
	}
	if (!Libs::LibKernel::Memory::TryWriteBacking(dst_range.address, out.data(),
	                                                total * sizeof(uint32_t))) {
		return false;
	}
	context.InvalidateMemory(dst_range.address, total * sizeof(uint32_t));
	return true;
}
} // namespace

void RenderExecutor::DispatchDirect(uint64_t submit_id, CommandBuffer& buffer,
                                    uint32_t thread_group_x, uint32_t thread_group_y,
                                    uint32_t thread_group_z, uint32_t mode, bool async_compute) {
	EXIT_IF(buffer.IsInvalid());
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), submit_id,
	                    thread_group_x, thread_group_y, thread_group_z, mode,
	                    sh_ctx.GetCs().cs_regs.data_addr);

	if (thread_group_x == 0 || thread_group_y == 0 || thread_group_z == 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: skipping zero-sized dispatch groups=%ux%ux%u "
			     "mode=0x%08" PRIx32 " shader=0x%016" PRIx64 "\n",
			     thread_group_x, thread_group_y, thread_group_z, mode,
			     sh_ctx.GetCs().cs_regs.data_addr);
		}
		return;
	}

	Common::LockGuard lock(m_context.GetMutex());
	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		LOGF("GraphicsRenderDispatchDirect: temporary: ignoring dispatch with null CS shader, "
		     "groups=%ux%ux%u mode=%u\n",
		     thread_group_x, thread_group_y, thread_group_z, mode);
		return;
	}

	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		return;
	}

	constexpr uint32_t DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS = 1u << 5u;
	constexpr uint32_t DISPATCH_INITIATOR_BASE_BITS             = 0x41u;
	constexpr uint32_t DISPATCH_INITIATOR_MODIFIER_BITS         = 0xa038u;
	constexpr uint32_t DISPATCH_INITIATOR_KNOWN_MASK =
	    DISPATCH_INITIATOR_BASE_BITS | DISPATCH_INITIATOR_MODIFIER_BITS;

	const uint32_t unknown_mode_bits = mode & ~DISPATCH_INITIATOR_KNOWN_MASK;
	if (unknown_mode_bits != 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: unknown dispatch initiator bits "
			     "mode=0x%08" PRIx32 " unknown=0x%08" PRIx32 " shader=0x%016" PRIx64
			     " groups=%ux%ux%u\n",
			     mode, unknown_mode_bits, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x,
			     thread_group_y, thread_group_z);
		}
	}

	const auto& cs_regs = sh_ctx.GetCs();
	const auto& sh_regs = ctx.GetShaderRegisters();

	ShaderComputeInputInfo input_info {};
	input_info.async_compute = async_compute;
	const bool use_thread_dimensions = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	input_info.dispatch_thread_dimensions = use_thread_dimensions;
	input_info.workgroup_counts[0] = thread_group_x;
	input_info.workgroup_counts[1] = thread_group_y;
	input_info.workgroup_counts[2] = thread_group_z;
	if (use_thread_dimensions) {
		const uint32_t group_sizes[] = {cs_regs.cs_regs.num_thread_x, cs_regs.cs_regs.num_thread_y,
		                                cs_regs.cs_regs.num_thread_z};
		for (uint32_t axis = 0; axis < 3u; ++axis) {
			const auto size = std::max(group_sizes[axis], 1u);
			const auto threads = input_info.workgroup_counts[axis];
			input_info.workgroup_counts[axis] = threads / size + (threads % size != 0u);
		}
	}
	const auto compute_program =
	    m_context.GetPipelineCache().GetComputeProgram(cs_regs, sh_regs, input_info);
	if (!compute_program) {
		// Shader was skipped (logged at the skip site); the game continues
		// without this dispatch.
		ResetBindings();
		return;
	}
	if (use_thread_dimensions) {
		input_info.dispatch_threads_num[0]    = thread_group_x;
		input_info.dispatch_threads_num[1]    = thread_group_y;
		input_info.dispatch_threads_num[2]    = thread_group_z;
	}

	const auto& program   = *input_info.stage.program;
	const auto& resources = *input_info.stage.resources;
	// Known-hang compute shaders: skipping lets the game continue instead of
	// hanging the GPU (TDR). 0x5d7ccd5dbcdd0c9f hangs amdvlk on first dispatch
	// in RE Engine streaming loads; the game runs fine without its results.
	// Additional hashes (one per line/space/comma, hex) can be supplied in
	// skip_cs_hashes.txt next to the executable for triage.
	static constexpr uint64_t kHungComputeHashes[] = {0x5d7ccd5dbcdd0c9full};
	static std::vector<uint64_t> extra_hashes;
	// Conditional entries: {hash, min_groups} skips only dispatches whose
	// x*y*z exceeds min_groups (file syntax HASH>MIN, MIN decimal).
	static std::vector<std::pair<uint64_t, uint64_t>> extra_hash_limits;
	static std::once_flag         hashes_once;
	std::call_once(hashes_once, [] {
		Common::File file;
		if (!file.Open(std::filesystem::path("skip_cs_hashes.txt"),
		               Common::File::Mode::Read)) {
			return;
		}
		const auto size = file.Size();
		if (size == 0 || size > 4096) {
			return;
		}
		std::string text(static_cast<size_t>(size), '\0');
		uint32_t    bytes_read = 0;
		file.Read(text.data(), static_cast<uint32_t>(size), &bytes_read);
		text.resize(bytes_read);
		for (char& c: text) {
			if (c == ',' || c == '\r' || c == '\n' || c == '\t') {
				c = ' ';
			}
		}
		for (const auto& word: Common::Split(text, ' ', false)) {
			if (word.empty()) {
				continue;
			}
			const auto gt = word.find('>');
			const std::string hash_part =
			    (gt == std::string::npos) ? word : word.substr(0, gt);
			char*        end  = nullptr;
			const uint64_t hash = std::strtoull(hash_part.c_str(), &end, 16);
			if (end == hash_part.c_str()) {
				continue;
			}
			if (gt != std::string::npos) {
				if (*end != '\0') {
					continue;
				}
				char*          lim_end = nullptr;
				const uint64_t limit =
				    std::strtoull(word.c_str() + gt + 1, &lim_end, 10);
				if (lim_end == word.c_str() + gt + 1 || *lim_end != '\0') {
					continue;
				}
				extra_hash_limits.emplace_back(hash, limit);
			} else if (*end == '\0') {
				extra_hashes.push_back(hash);
			}
		}
		if (!extra_hashes.empty() || !extra_hash_limits.empty()) {
			std::printf("DispatchDirect: loaded %zu extra skip hashes + %zu size-limited\n",
			            extra_hashes.size(), extra_hash_limits.size());
			std::fflush(stdout);
		}
	});
	for (const auto hung : kHungComputeHashes) {
		if (program.shader_hash == hung) {
			static std::atomic_uint hung_logged = 0;
			if (hung_logged.fetch_add(1, std::memory_order_relaxed) < 3) {
				std::printf("DispatchDirect: skipping known-hang shader 0x%016llx\n",
				            static_cast<unsigned long long>(program.shader_hash));
				std::fflush(stdout);
			}
			ResetBindings();
			return;
		}
	}
	for (const auto hung : extra_hashes) {
		if (program.shader_hash == hung) {
			ResetBindings();
			return;
		}
	}
	{
		const uint64_t groups = static_cast<uint64_t>(thread_group_x) * thread_group_y *
		                        thread_group_z;
		for (const auto& [hung, min_groups]: extra_hash_limits) {
			if (program.shader_hash == hung && groups > min_groups) {
				static std::atomic_uint limit_logged {0};
				if (limit_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
					std::printf("DispatchDirect: size-skipped hash=0x%016llx groups=%ux%ux%u\n",
					            static_cast<unsigned long long>(program.shader_hash),
					            thread_group_x, thread_group_y, thread_group_z);
					std::fflush(stdout);
				}
				ResetBindings();
				return;
			}
		}
	}
	// One line per dispatch (printf so it lands in the run log): submit ids
	// in submit-failure reports map back to the exact shader hash here.
	// Placed after the skip checks so only executed dispatches are logged.
	std::printf("DispatchDirect: submit=%llu shader_hash=0x%016llx groups=%ux%ux%u mode=0x%x wave=%u\n",
	            static_cast<unsigned long long>(submit_id),
	            static_cast<unsigned long long>(program.shader_hash), thread_group_x,
	            thread_group_y, thread_group_z, mode, program.wave_size);
	// TEMP-DIAG: insane dispatch sizes hang the GPU (TDR) with no other trace.
	{
		const uint64_t total = static_cast<uint64_t>(thread_group_x) * thread_group_y * thread_group_z;
		if (total > 100000000ull) {
			std::printf("SUSPICIOUS DISPATCH: submit=%llu shader_hash=0x%016llx groups=%ux%ux%u\n",
			            static_cast<unsigned long long>(submit_id),
			            static_cast<unsigned long long>(program.shader_hash), thread_group_x,
			            thread_group_y, thread_group_z);
			std::fflush(stdout);
		}
	}
	if (resources.specialization_reads.empty() &&
	    (TryConsumeComputeMetaClear(input_info, buffer) ||
	     TryConsumeComputeImageClear(input_info, buffer, thread_group_x, thread_group_y,
	                                 thread_group_z, mode))) {
		ResetBindings();
		return;
	}

	const bool large_workgroup =
	    (input_info.threads_num[0] * input_info.threads_num[1] * input_info.threads_num[2] >= 512);
	const bool                   has_sampler = !program.info.samplers.empty();
	static std::atomic<uint32_t> dispatch_log_count {0};
	// Always detail-log the two kernels dominating TDR death batches.
	const bool tdr_suspect = program.shader_hash == 0xdd7d9b716cfe5abaull ||
	                         program.shader_hash == 0x0b3ee2d73a90eeb6ull;
	static std::atomic<uint32_t> suspect_log_count {0};
	const bool suspect_sample =
	    tdr_suspect && (suspect_log_count.load(std::memory_order_relaxed) < 30 ||
	                    (submit_id % 100 == 0 && suspect_log_count.load(std::memory_order_relaxed) < 3000));
	if (suspect_sample) {
		suspect_log_count.fetch_add(1, std::memory_order_relaxed);
	}
	const bool detail_log_sample =
	    ((large_workgroup || has_sampler) &&
	     dispatch_log_count.fetch_add(1, std::memory_order_relaxed) < 512) ||
	    suspect_sample;
	if (use_thread_dimensions) {
		const uint32_t old_x = thread_group_x;
		const uint32_t old_y = thread_group_y;
		const uint32_t old_z = thread_group_z;
		thread_group_x       = input_info.workgroup_counts[0];
		thread_group_y       = input_info.workgroup_counts[1];
		thread_group_z       = input_info.workgroup_counts[2];

		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: use-thread-dimensions %ux%ux%u / %ux%ux%u -> "
			     "groups %ux%ux%u\n",
			     old_x, old_y, old_z, std::max(cs_regs.cs_regs.num_thread_x, 1u),
			     std::max(cs_regs.cs_regs.num_thread_y, 1u),
			     std::max(cs_regs.cs_regs.num_thread_z, 1u), thread_group_x, thread_group_y,
			     thread_group_z);
		}
	}

	buffer.EndRendering();
	auto& pipeline =
	    m_context.GetPipelineCache().GetComputePipeline(input_info, compute_program);
	if (pipeline.pipeline == nullptr) {
		// Creation failed and was logged; skip the dispatch so the game continues.
		return;
	}
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	if (program.bindings.dispatch_thread_dword != ShaderRecompiler::IR::PushData::NoStart) {
		std::copy(std::begin(input_info.dispatch_threads_num), std::end(input_info.dispatch_threads_num),
		          bindings.shader_data.begin() + program.bindings.dispatch_thread_dword);
	}
	PreparedBindings* descriptor_stage = &bindings;
	FindBuffers(std::span {&descriptor_stage, 1u});
	if (program.info.uses_dma) {
		m_context.PrepareBda();
	}
	RebindImages(bindings);
	BindSharedMemory(m_context, input_info, bindings);
	RebindBuffers(bindings);

	auto              vk_buffer        = buffer.Handle();
	// RE Engine Maiden: kernel 0xdd7d9b716cfe5aba renders menus fine but hangs
	// the GPU in the post-start streaming scene. Once the user pressed Cross,
	// emulate it on the CPU (exact memcpy semantics) instead of executing it;
	// if the shape is unexpected, skip it as before.
	// TEMP-DIAG: emulate every dd7d (logs shape) but only divert post-start.
	if (program.shader_hash == 0xdd7d9b716cfe5abaull) {
		static std::atomic_uint emu_logged {0};
		const bool             emulated = CpuEmulateStreamingCopy(
                m_context, input_info, program, bindings, thread_group_x, thread_group_y,
                thread_group_z);
		if (emu_logged.fetch_add(1, std::memory_order_relaxed) < 4) {
			std::printf("DispatchDirect: dd7d %s (groups=%ux%ux%u started=%d)\n",
			            emulated ? "cpu-emulated" : "skip-refused",
			            thread_group_x, thread_group_y, thread_group_z,
			            Libs::Controller::GameStartPressed() ? 1 : 0);
			std::fflush(stdout);
		}
		if (Libs::Controller::GameStartPressed()) {
			ResetBindings();
			return;
		}
	}
	if (!CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	                      std::span {&descriptor_stage, 1u})) {
		ResetBindings();
		return;
	}
	bool has_storage_writes = bindings.shared_memory.buffer != nullptr ||
	    HasShaderBufferWrites(input_info.stage);
	has_storage_writes =
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& image) {
		                return image.written &&
		                       image.resource_class ==
		                           ShaderRecompiler::IR::ImageResourceClass::Storage;
	                }) ||
	    has_storage_writes;
	if (has_storage_writes) {
		// A host fence used to serialize every dispatch. Preserve its read-before-write ordering
		// while allowing the queue to execute asynchronously.
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	if (Config::GraphicsDebugDumpEnabled() || detail_log_sample) {
		const auto sampled_images = std::count_if(
		    program.info.images.begin(), program.info.images.end(), [](const auto& image) {
			    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
		    });
		const uint32_t frame_num = static_cast<uint32_t>(m_context.GetGpu().GetFrameNum());
		LOGF("GraphicsRenderDispatchDirect: frame=%u shader=0x%016" PRIx64
		     " hash=0x%016" PRIx64 " tick=%" PRIu64
		     " groups=%ux%ux%u mode=0x%08" PRIx32 " local=%ux%ux%u "
		     "buffers=%zu textures=%zu sampled=%zu storage=%zu samplers=%zu push=%u\n",
		     frame_num, sh_ctx.GetCs().cs_regs.data_addr, program.shader_hash,
		     m_context.GetCommandScheduler().CurrentTick(),
		     thread_group_x, thread_group_y, thread_group_z, mode,
		     input_info.threads_num[0], input_info.threads_num[1],
		     input_info.threads_num[2], program.info.buffers.size(), program.info.images.size(),
		     sampled_images, program.info.images.size() - sampled_images,
		     program.info.samplers.size(),
		     program.bindings.UsesPushData()
		         ? static_cast<uint32_t>(sizeof(ShaderRecompiler::IR::PushData))
		         : 0u);
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& buffer = program.info.buffers[i];
			const auto  r      = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			LOGF("  CS buffer[%u]: source=%u usage=%s addr=0x%012" PRIx64
			     " stride=%u records=%u format=%u\n",
			     i, buffer.source, buffer.written ? "read-write" : "read-only", r.Base48(),
			     r.Stride(), r.NumRecords(), r.RawFormat());
		}
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			const auto& image = program.info.images[i];
			const auto  r     = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
			LOGF("  CS texture[%u]: source=%u usage=%s sampled=%s addr=0x%010" PRIx64
			     " type=%u fmt=%u extent=%ux%u depth=%u levels=%u tile=%u\n",
			     i, image.source, image.written ? "read-write" : "read-only",
			     image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled
			         ? "true"
			         : "false",
			     r.Base40(), static_cast<uint32_t>(r.Type()), static_cast<uint32_t>(r.Format()),
			     static_cast<uint32_t>(r.Width5()) + 1u, static_cast<uint32_t>(r.Height5()) + 1u,
			     static_cast<uint32_t>(r.Depth()) + 1u,
			     r.Type() == Prospero::ImageType::kColor2DMsaa ||
			             r.Type() == Prospero::ImageType::kColor2DMsaaArray
			         ? 1u
			         : static_cast<uint32_t>(image.r128 ? r.LastLevel() : r.MaxMip()) + 1u,
			     static_cast<uint32_t>(r.TileMode()));
		}
		for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
			const auto r = DecodeNativeDescriptor<ShaderSamplerResource>(
			    resources.samplers[program.info.samplers[i].snapshot_index]);
			LOGF("  CS sampler[%u]: source=%u clamp=%u/%u/%u filter=%u/%u/%u mip=%u "
			     "lod=%u-%u bias=%d\n",
			     i, program.info.samplers[i].source, static_cast<uint32_t>(r.ClampX()),
			     static_cast<uint32_t>(r.ClampY()), static_cast<uint32_t>(r.ClampZ()),
			     static_cast<uint32_t>(r.XyMagFilter()), static_cast<uint32_t>(r.XyMinFilter()),
			     static_cast<uint32_t>(r.ZFilter()), static_cast<uint32_t>(r.MipFilter()),
			     static_cast<uint32_t>(r.MinLod()), static_cast<uint32_t>(r.MaxLod()),
			     static_cast<int32_t>(r.LodBias()));
		}
	}


	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	vk_buffer.dispatch(thread_group_x, thread_group_y, thread_group_z);

	// The removed host fence also ordered read-only dispatches before later writers.
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	ResetBindings();
	SplitOversizedSubmit();
}

void RenderExecutor::SplitOversizedSubmit() {
	// Batches of hundreds of dispatches in one vkQueueSubmit hang some drivers;
	// normal phases record an order of magnitude fewer (verified on RE Engine).
	static constexpr uint32_t kMaxDispatchesPerSubmit = 64;
	if (++m_dispatches_since_submit >= kMaxDispatchesPerSubmit) {
		m_dispatches_since_submit = 0;
		m_context.GetCommandScheduler().Flush();
	}
}

void RenderExecutor::DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer,
                                      uint64_t args_addr, uint32_t mode, bool async_compute) {
	EXIT_IF(buffer.IsInvalid() || args_addr == 0 || (args_addr & 3u) != 0 ||
	        (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchIndirect), submit_id,
	                    static_cast<uint32_t>(args_addr), static_cast<uint32_t>(args_addr >> 32u),
	                    0, mode, buffer.GetShaders().GetCs().cs_regs.data_addr);
	Common::LockGuard lock(m_context.GetMutex());
	const auto& cs_regs = buffer.GetShaders().GetCs();
	if (cs_regs.cs_regs.data_addr == 0) {
		return;
	}
	ShaderComputeInputInfo input_info {};
	input_info.async_compute = async_compute;
	const auto compute_program = m_context.GetPipelineCache().GetComputeProgram(
	    cs_regs, buffer.GetRegisters().GetShaderRegisters(), input_info);
	if (!compute_program) {
		// Shader was skipped (logged at the skip site); the game continues
		// without this dispatch.
		ResetBindings();
		return;
	}
	buffer.EndRendering();
	auto& pipeline = m_context.GetPipelineCache().GetComputePipeline(input_info, compute_program);
	if (pipeline.pipeline == nullptr) {
		// Creation failed and was logged; skip the dispatch so the game continues.
		return;
	}
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	PreparedBindings* descriptor_stage = &bindings;
	FindBuffers(std::span {&descriptor_stage, 1u});
	const auto& program = *input_info.stage.program;
	if (program.info.uses_dma) {
		m_context.PrepareBda();
	}
	BindSharedMemory(m_context, input_info, bindings, args_addr);
	RebindImages(bindings);
	// Acquiring arguments can merge cache buffers; finalize shader bindings afterward.
	const auto [args_buffer, args_offset] = m_context.GetBufferCache().ObtainBuffer(
	    args_addr, sizeof(vk::DispatchIndirectCommand), false);
	EXIT_IF(args_buffer == nullptr || (args_offset & 3u) != 0);
	// Log indirect counts (best effort: GPU-produced args may be stale on the
	// CPU side, but CPU-produced args show exactly).
	{
		uint32_t   indirect_xyz[3] = {};
		const auto mapped          = args_buffer->Mapped();
		if (args_offset + sizeof(indirect_xyz) <= mapped.size()) {
			std::memcpy(indirect_xyz, mapped.data() + args_offset, sizeof(indirect_xyz));
		}
		std::printf("DispatchIndirect: submit=%llu shader_hash=0x%016llx args_addr=0x%llx "
		            "xyz=%ux%ux%u mode=0x%x buf=0x%llx+%llx mapped=%zu\n",
		            static_cast<unsigned long long>(submit_id),
		            static_cast<unsigned long long>(program.shader_hash),
		            static_cast<unsigned long long>(args_addr), indirect_xyz[0], indirect_xyz[1],
		            indirect_xyz[2], mode,
		            static_cast<unsigned long long>(args_buffer->CpuAddress()),
		            static_cast<unsigned long long>(args_buffer->Size()), mapped.size());
		std::fflush(stdout);
		// CPU-visible garbage counts hang the GPU (TDR) with no other trace;
		// skip instead of hanging. Legit dispatches stay far below this.
		if (mapped.size() > 0) {
			const uint64_t total = static_cast<uint64_t>(indirect_xyz[0]) * indirect_xyz[1] *
			                       indirect_xyz[2];
			if (total > (1ull << 24)) {
				std::printf("SKIP insane indirect: submit=%llu shader_hash=0x%016llx "
				            "xyz=%ux%ux%u\n",
				            static_cast<unsigned long long>(submit_id),
				            static_cast<unsigned long long>(program.shader_hash),
				            indirect_xyz[0], indirect_xyz[1], indirect_xyz[2]);
				std::fflush(stdout);
				ResetBindings();
				return;
			}
		}
	}
	RebindBuffers(bindings);
	if (program.shader_hash == 0xdd7d9b716cfe5abaull &&
	    Libs::Controller::GameStartPressed()) {
		ResetBindings();
		return;
	}
	if (!CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	                      std::span {&descriptor_stage, 1u})) {
		ResetBindings();
		return;
	}
	const auto vk_buffer = buffer.Handle();
	const bool has_storage_writes = bindings.shared_memory.buffer != nullptr ||
	    HasShaderBufferWrites(input_info.stage) ||
	    std::any_of(program.info.images.begin(), program.info.images.end(), [](const auto& image) {
		    return image.written && image.resource_class ==
		                                ShaderRecompiler::IR::ImageResourceClass::Storage;
	    });
	if (has_storage_writes) {
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics |
	                              vk::PipelineStageFlagBits::eComputeShader |
	                              vk::PipelineStageFlagBits::eTransfer,
	                          vk::PipelineStageFlagBits::eDrawIndirect, {},
	                          1, &barrier, 0, nullptr, 0, nullptr);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	vk_buffer.dispatchIndirect(args_buffer->Handle(), args_offset);
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	ResetBindings();
	SplitOversizedSubmit();
}

} // namespace Libs::Graphics
