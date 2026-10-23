#include "graphics/host_gpu/renderer/renderDraw.h"

#include "common/assert.h"

#include <chrono>
#include "common/common.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

std::pair<int32_t, uint32_t> ResolveDrawOffsets(uint32_t index_offset,
	                                           const ShaderVertexInputInfo& vs_input_info) {
	auto     vertex_offset   = static_cast<int32_t>(index_offset);
	uint32_t instance_offset = 0;
	if (!vs_input_info.fetch_embedded) {
		return {vertex_offset, instance_offset};
	}

	EXIT_IF(!vs_input_info.stage);
	const auto& program   = *vs_input_info.stage.program;
	const auto& resources = *vs_input_info.stage.resources;
	if (index_offset == 0 &&
	    program.info.vertex_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.vertex_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			vertex_offset = static_cast<int32_t>(resources.user_data[index]);
		}
	}
	if (program.info.instance_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.instance_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			instance_offset = resources.user_data[index];
		}
	}

	return {vertex_offset, instance_offset};
}

static std::atomic<uint32_t> g_draw_state_log_count   = 0;

// Guest video-out / scanout buffers live at 0x8fc.... in the 40-bit guest
// address space. The top twelve bits identify them; shifting by 32 is wrong
// because that yields 0x8f for 0x8fc0000000, so the buffer used to slip
// through every "skip the video-out target" check.
static constexpr uint64_t kVideoOutAddressMask = 0xFFF0000000ull;
static constexpr uint64_t kVideoOutAddressBase = 0x8FC0000000ull;

[[nodiscard]] static bool IsVideoOutAddress(uint64_t address) {
	return (address & kVideoOutAddressMask) == kVideoOutAddressBase;
}

// Scene-target tracking for the composite override: remember every display-
// shaped guest colour target that was actually written, together with its
// format and backing image. The composite then prefers the freshest HDR scene
// target over the LDR composite-source target (which is often never filled).
static std::mutex g_scene_target_mutex {};
struct SceneTargetStats {
	uint32_t   draws    = 0;
	uint32_t   width    = 0;
	uint32_t   height   = 0;
	uint64_t   last_seq = 0;
	ImageId    image_id {};
	vk::Format format = vk::Format::eUndefined;
};
static std::unordered_map<uint64_t, SceneTargetStats> g_scene_target_draws {};
static uint64_t                                       g_scene_target_seq = 0;
// Guest address of the composite's scene source, learned from the composite
// draw. Diagnostics watch draws into this address to find the tonemap pass.
static std::atomic<uint64_t> g_composite_source_addr {0};

// See render.h: lets the presenter know what a video-out buffer currently holds.
std::atomic<uint64_t> g_composite_override_tick {0};
std::atomic<uint64_t> g_video_out_linear_addr[kVideoOutLinearSlots];
std::atomic<int64_t>  g_video_out_linear_frame[kVideoOutLinearSlots];

// A guest frame that starts drawing into a video-out buffer replaces whatever the previous
// frame left there, so the linear-scene marker no longer describes its contents.
static void ClearVideoOutLinearBeforeFrame(uint64_t target_addr, int64_t frame) {
	for (size_t slot = 0; slot < kVideoOutLinearSlots; ++slot) {
		if (g_video_out_linear_addr[slot].load(std::memory_order_acquire) != target_addr) {
			continue;
		}
		if (g_video_out_linear_frame[slot].load(std::memory_order_acquire) < frame) {
			g_video_out_linear_frame[slot].store(0, std::memory_order_release);
		}
		return;
	}
}

// Records the guest frame in which a video-out buffer last received the linear scene
// target (frame > 0), or that it now holds the guest's own composite again (frame == 0).
// Keyed by address so both members of the guest's double buffer keep their own state.
static void SetVideoOutLinearFrame(uint64_t target_addr, int64_t frame) {
	if (target_addr == 0) {
		return;
	}
	for (size_t slot = 0; slot < kVideoOutLinearSlots; ++slot) {
		if (g_video_out_linear_addr[slot].load(std::memory_order_acquire) == target_addr) {
			g_video_out_linear_frame[slot].store(frame, std::memory_order_release);
			return;
		}
	}
	if (frame == 0) {
		return;
	}
	for (size_t slot = 0; slot < kVideoOutLinearSlots; ++slot) {
		uint64_t expected = 0;
		if (g_video_out_linear_addr[slot].compare_exchange_strong(
		        expected, target_addr, std::memory_order_acq_rel)) {
			g_video_out_linear_frame[slot].store(frame, std::memory_order_release);
			return;
		}
	}
	static std::atomic_uint next_slot {0};
	const auto slot = next_slot.fetch_add(1, std::memory_order_relaxed) % kVideoOutLinearSlots;
	g_video_out_linear_addr[slot].store(target_addr, std::memory_order_release);
	g_video_out_linear_frame[slot].store(frame, std::memory_order_release);
}
// Tonemap input images learned from the tonemap draw, shared with the compute
// path so we can find which dispatches (if any) write them.
namespace KytySceneDiag {
std::atomic<uint64_t> exposure_addr {0};
std::atomic<uint64_t> lut_addr {0};
} // namespace KytySceneDiag

[[nodiscard]] static bool IsHdrSceneFormat(vk::Format format) {
	switch (format) {
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR32G32B32A32Sfloat:
		case vk::Format::eR16G16B16Sfloat:
		case vk::Format::eR32G32B32Sfloat:
		case vk::Format::eB10G11R11UfloatPack32:
		case vk::Format::eE5B9G9R9UfloatPack32: return true;
		default: return false;
	}
}

struct SceneTargetPick {
	uint64_t   address  = 0;
	uint32_t   width    = 0;
	uint32_t   height   = 0;
	ImageId    image_id {};
	vk::Format format = vk::Format::eUndefined;
	uint32_t   draws    = 0;
	uint64_t   last_seq = 0;
};

[[nodiscard]] static SceneTargetPick PickSceneTarget(uint64_t            exclude_address,
                                                     const vk::Extent2D& want_extent) {
	std::lock_guard<std::mutex> lock {g_scene_target_mutex};
	SceneTargetPick             best {};
	// The game switches scene colour targets over its lifetime, so recency
	// decides. Rank 3 is an HDR target at the composite's own resolution (the
	// scene colour buffer), rank 2 any other HDR target, rank 1 an LDR target.
	int      best_rank = 0;
	uint64_t best_seq  = 0;
	for (const auto& [address, stats]: g_scene_target_draws) {
		if (address == exclude_address) {
			continue;
		}
		int rank = 1;
		if (IsHdrSceneFormat(stats.format)) {
			rank = (want_extent.width != 0 && stats.width == want_extent.width &&
			        stats.height == want_extent.height)
			           ? 3
			           : 2;
		}
		if (rank > best_rank || (rank == best_rank && stats.last_seq > best_seq)) {
			best_rank     = rank;
			best_seq      = stats.last_seq;
			best.address  = address;
			best.width    = stats.width;
			best.height   = stats.height;
			best.image_id = stats.image_id;
			best.format   = stats.format;
			best.draws    = stats.draws;
			best.last_seq = stats.last_seq;
		}
	}
	return best;
}

[[nodiscard]] static SceneTargetStats SceneTargetStatsFor(uint64_t address) {
	std::lock_guard<std::mutex> lock {g_scene_target_mutex};
	const auto                  it = g_scene_target_draws.find(address);
	return it == g_scene_target_draws.end() ? SceneTargetStats {} : it->second;
}

static void NoteSceneTargetWrite(uint64_t address, const vk::Extent2D& extent,
                                 ImageId image_id, vk::Format format) {
	// Only large, display-shaped colour targets qualify as scene targets. Skip
	// the video-out/scanout buffers themselves, tiny UI targets and square
	// shadow targets (they used to win the "most writes" contest).
	if (address == 0 || IsVideoOutAddress(address) || extent.width < 1280 ||
	    extent.height < 720 ||
	    static_cast<uint64_t>(extent.width) * 10ull <
	        static_cast<uint64_t>(extent.height) * 16ull ||
	    static_cast<uint64_t>(extent.width) * 10ull >
	        static_cast<uint64_t>(extent.height) * 20ull) {
		return;
	}
	std::lock_guard<std::mutex> lock {g_scene_target_mutex};
	auto&                       stats = g_scene_target_draws[address];
	++stats.draws;
	stats.width    = extent.width;
	stats.height   = extent.height;
	stats.image_id = image_id;
	stats.format   = format;
	stats.last_seq = ++g_scene_target_seq;
	static std::atomic_uint new_logged {0};
	if (stats.draws == 1 && new_logged.fetch_add(1, std::memory_order_relaxed) < 100) {
		LOGF("SceneTargetNew: addr=0x%010" PRIx64 " ext=%ux%u fmt=%s id=%u/%u\n", address,
		     extent.width, extent.height, vk::to_string(format).c_str(),
		     image_id.index, image_id.generation);
	}
}


static std::atomic<uint32_t> g_draw_input_log_count   = 0;
static std::atomic<uint32_t> g_mrt_state_log_count    = 0;

static std::atomic<uint32_t> g_framebuffer_skip_log_count = 0;

// Always-on draw census: indexed vs auto draws, silent skip reasons, and the
// last completed draw's target. Cheap (one log per 1000 completed draws) so
// real-game runs stay at full speed while remaining diagnosable.
static std::atomic<uint64_t> g_census_index_calls   = 0;
static std::atomic<uint64_t> g_census_auto_calls    = 0;
static std::atomic<uint64_t> g_census_empty         = 0;
static std::atomic<uint64_t> g_census_resolved      = 0;
static std::atomic<uint64_t> g_census_no_vs         = 0;
static std::atomic<uint64_t> g_census_topology_fail = 0;
static std::atomic<uint64_t> g_census_state_fail    = 0;
static std::atomic<uint64_t> g_census_rectlist_skip = 0;
static std::atomic<uint64_t> g_census_mesh_skip      = 0;
static std::atomic<uint64_t> g_census_pipeline_null = 0;
static std::atomic<uint64_t> g_census_complete      = 0;

static float ConvertPolygonOffsetConstantFactor(float guest_factor, const HW::PolyOffset& offset,
                                                vk::Format host_depth_format) {
	if (offset.db_is_float_fmt) {
		return guest_factor;
	}

	int host_depth_bits = 0;
	switch (host_depth_format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint: host_depth_bits = 16; break;
		case vk::Format::eD24UnormS8Uint: host_depth_bits = 24; break;
		default:
			// A fixed-point guest bias cannot be represented exactly by a floating-point host
			// attachment without VK_EXT_depth_bias_control.
			return guest_factor;
	}
	return std::ldexp(guest_factor, host_depth_bits + offset.neg_num_db_bits);
}

static const char* RenderColorTypeName(const RenderColorInfo& color) {
	return color.image_id ? "RenderTexture" : "NoColorOutput";
}

static void LogFramebufferSkip(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               uint32_t index_count, uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	auto log_id = g_framebuffer_skip_log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 128) {
		return;
	}

	LOGF(
	    "DrawFramebufferSkip[%u]: %s color=%s color_addr=0x%010" PRIx64 " color_size=0x%016" PRIx64
	    " color_image=%s depth_format=%s depth_image=%s depth_vaddr_num=%d target_mask=0x%08" PRIx32
	    " prim=%u index_count=%u flags=0x%08" PRIx32 "\n",
	    log_id, draw_name, RenderColorTypeName(color), color.desc.info.data.address,
	    color.desc.info.data.size, color.image_id ? "yes" : "no",
	    vk::to_string(depth.desc.view_info.format).c_str(), depth.image_id ? "yes" : "no",
	    static_cast<int>(!depth.desc.info.data.Empty()) +
	        static_cast<int>(depth.desc.info.HasStencil()),
	    ctx.GetRenderTargetMask(), static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags);
}

static void LogMrtState(const char* draw_name, const CommandBuffer& buffer,
                        const ShaderPixelInputInfo& ps_input_info) {
	const auto& ctx            = buffer.GetRegisters();
	const auto& sh_regs        = ctx.GetShaderRegisters();
	const auto  rt_mask        = ctx.GetRenderTargetMask();
	const auto  cb_shader_mask = sh_regs.m_cbShaderMask;
	const auto& bc0            = ctx.GetBlendControl(0);

	auto log_id = g_mrt_state_log_count.fetch_add(1);
	if (log_id >= 32) {
		return;
	}

	LOGF("MrtState[%u]: %s rt_mask=0x%08" PRIx32 " cb_shader_mask=0x%08" PRIx32
	     " blend0=%s src=%u dst=%u alpha_src=%u alpha_dst=%u sep_alpha=%s\n",
	     log_id, draw_name, rt_mask, cb_shader_mask, bc0.enable ? "true" : "false",
	     bc0.color_srcblend, bc0.color_destblend, bc0.alpha_srcblend, bc0.alpha_destblend,
	     bc0.separate_alpha_blend ? "true" : "false");

	for (uint32_t i = 0; i < 8; i++) {
		const auto& rt  = ctx.GetRenderTarget(i);
		const auto& bc  = ctx.GetBlendControl(i);
		const auto  ctm = (rt_mask >> (i * 4u)) & 0x0fu;
		const auto  csm = (cb_shader_mask >> (i * 4u)) & 0x0fu;

		if (rt.base.addr == 0 && ps_input_info.target_output_mode[i] == 0 && ctm == 0 && csm == 0 &&
		    !bc.enable) {
			continue;
		}

		LOGF("MrtState[%u]: slot=%u addr=0x%010" PRIx64
		     " target_mask=0x%x shader_mask=0x%x out_mode=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32
		     " width=%u height=%u tile=%u"
		     " blend=%s src=%u dst=%u alpha_src=%u alpha_dst=%u\n",
		     log_id, i, rt.base.addr, ctm, csm, ps_input_info.target_output_mode[i],
		     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
		     static_cast<uint32_t>(rt.info.channel_order), rt.attrib2.width + 1,
		     rt.attrib2.height + 1, static_cast<uint32_t>(rt.attrib3.tile_mode),
		     bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.alpha_srcblend,
		     bc.alpha_destblend);
	}
}

static void LogDrawTargetState(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               const ShaderPixelInputInfo& ps_input_info, uint32_t index_count,
                               uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!color.image_id) {
		return;
	}

	// Always log draws into the screen/video-out region so the final composite
	// (scene -> present buffer) can be diagnosed regardless of sample rate.
	const bool screen_target = IsVideoOutAddress(color.desc.info.data.address);
	auto log_id = g_draw_state_log_count.fetch_add(1);
	// Log the first draws (boot) under the full debug dump, or periodic samples
	// when only the lightweight sample switch is on: the first 192 only ever
	// show loading/menu draws, never the 3D scene, so a black world cannot be
	// diagnosed from them.
	if (graphics_debug_dump_enabled()) {
		if (log_id >= 192u && !screen_target) {
			return;
		}
	} else if (!screen_target && !((log_id % 20000u) == 0u && log_id < 20000u * 200u)) {
		return;
	}

	const auto& cc             = ctx.GetColorControl();
	const auto& bc             = ctx.GetBlendControl(color.target_slot);
	const auto& dc             = ctx.GetDepthControl();
	const auto& vp             = ctx.GetScreenViewport();
	const auto& vp0            = vp.viewports[0];
	const auto& ps_resources   = ps_input_info.stage.program->info;
	const auto  sampled_images = std::count_if(
	    ps_resources.images.begin(), ps_resources.images.end(), [](const auto& image) {
		    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
	    });

	const auto extent = color.Extent();
	const auto sc     = calc_final_scissor(vp, ctx.GetScanModeControl(), extent, 0);

	LOGF(
	    "DrawTargetState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64
	    " extent=%ux%u prim=%u index_count=%u flags=0x%08" PRIx32 " color_mask=0x%08" PRIx32
	    " cc_mode=%u cc_op=0x%02x"
	    " blend=%s src=%u dst=%u comb=%u ps_tex=%d sampled=%d storage=%d ps_kill=%s target_mode0=%u"
	    " depth_test=%s depth_write=%s depth_func=%u depth_clear=%s depth_bound=%s"
	    " depth_cmp=%u depth_clear_val=%.2f viewport=(%.1f,%.1f %.1fx%.1f) "
	    "scissor=(%d,%d)-(%d,%d)\n",
	    log_id, buffer.GetContext().GetGpu().GetFrameNum(), draw_name, RenderColorTypeName(color),
	    color.desc.info.data.address, extent.width, extent.height,
	    static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags, ctx.GetRenderTargetMask(),
	    cc.mode, cc.op,
	    bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.color_comb_fcn,
	    static_cast<int>(ps_resources.images.size()), static_cast<int>(sampled_images),
	    static_cast<int>(ps_resources.images.size() - sampled_images),
	    ps_input_info.ps_pixel_kill_enable ? "true" : "false", ps_input_info.target_output_mode[0],
	    dc.z_enable ? "true" : "false", dc.z_write_enable ? "true" : "false", dc.zfunc,
	    depth.depth_clear_enable ? "true" : "false", depth.image_id ? "true" : "false",
	    static_cast<uint32_t>(depth.depth_compare_op),
	    static_cast<double>(depth.depth_clear_value), vp0.xoffset - vp0.xscale,
	    vp0.yoffset - vp0.yscale, vp0.xscale * 2.0f, vp0.yscale * 2.0f, sc.left, sc.top, sc.right,
	    sc.bottom);

	LogMrtState(draw_name, buffer, ps_input_info);
}

static void LogDrawInputState(const CommandBuffer& buffer, const RenderColorInfo& color,
                              const ShaderVertexInputInfo& vs_input_info,
                              uint32_t index_type_and_size, uint32_t index_count,
                              const void* index_addr) {
	auto log_id = g_draw_input_log_count.fetch_add(1);
	if (log_id >= 64) {
		return;
	}

	LOGF("DrawInputState[%u]: frame=%d target=%s addr=0x%010" PRIx64
	     " index_type=%u index_count=%u index_addr=0x%016" PRIx64
	     " vs_resources=%d vs_buffers=%d\n",
	     log_id, buffer.GetContext().GetGpu().GetFrameNum(), RenderColorTypeName(color),
	     color.desc.info.data.address, index_type_and_size, index_count,
	     reinterpret_cast<uint64_t>(index_addr), vs_input_info.resources_num,
	     vs_input_info.buffers_num);

	for (int bi = 0; bi < vs_input_info.buffers_num; bi++) {
		const auto& b = vs_input_info.buffers[bi];
		LOGF("DrawInputState[%u]: vb[%d] addr=0x%010" PRIx64
		     " stride=%u records=%u fetch_index=%u\n",
		     log_id, bi, b.addr, b.stride, b.num_records, b.fetch_index);

		const auto* bytes = reinterpret_cast<const uint8_t*>(b.addr);
		if (bytes != nullptr && b.stride != 0) {
			const uint32_t records = std::min<uint32_t>(b.num_records, 4u);
			for (uint32_t rec = 0; rec < records; rec++) {
				const auto* rec_bytes = bytes + static_cast<uint64_t>(rec) * b.stride;
				const auto  dword_num = std::min<uint32_t>(b.stride / 4u, 12u);
				uint32_t    raw[12]   = {};
				float       flt[12]   = {};
				for (uint32_t i = 0; i < dword_num; i++) {
					std::memcpy(&raw[i], rec_bytes + i * 4u, sizeof(raw[i]));
					std::memcpy(&flt[i], rec_bytes + i * 4u, sizeof(flt[i]));
				}
				LOGF("DrawInputState[%u]: vb[%d].rec[%u] stride=%u dwords=%u raw=%08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " f=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f)\n",
				     log_id, bi, rec, b.stride, dword_num, raw[0], raw[1], raw[2], raw[3], raw[4],
				     raw[5], raw[6], raw[7], raw[8], flt[0], flt[1], flt[2], flt[3], flt[4], flt[5],
				     flt[6], flt[7], flt[8]);

				for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
					const auto& r  = vs_input_info.resources[ai];
					const auto& rd = vs_input_info.resources_dst[ai];
					if (rd.buffer_index != bi) {
						continue;
					}
					const auto offset = static_cast<uint32_t>(r.Base48() - b.addr);
					if (offset + 4u <= b.stride &&
					    r.Format() == Prospero::BufferFormat::k8_8_8_8UNorm) {
						uint32_t packed = 0;
						std::memcpy(&packed, rec_bytes + offset, sizeof(packed));
						const auto r8 = (packed >> 0u) & 0xffu;
						const auto g8 = (packed >> 8u) & 0xffu;
						const auto b8 = (packed >> 16u) & 0xffu;
						const auto a8 = (packed >> 24u) & 0xffu;
						LOGF("DrawInputState[%u]: vb[%d].rec[%u].attr[%d] dst=v%d fmt=56 "
						     "rgba8=%02" PRIx32 "%02" PRIx32 "%02" PRIx32 "%02" PRIx32
						     " rgba=(%.3f,%.3f,%.3f,%.3f)\n",
						     log_id, bi, rec, ai, rd.register_start, r8, g8, b8, a8,
						     static_cast<double>(r8) / 255.0, static_cast<double>(g8) / 255.0,
						     static_cast<double>(b8) / 255.0, static_cast<double>(a8) / 255.0);
					}
				}
			}
		}

		for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
			const auto& r  = vs_input_info.resources[ai];
			const auto& rd = vs_input_info.resources_dst[ai];
			if (rd.buffer_index != bi) {
				continue;
			}
			LOGF("DrawInputState[%u]: attr[%d] offset=%u dst=v%d regs=%d fetch_index=%u "
			     "sharp=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			     log_id, ai, static_cast<uint32_t>(r.Base48() - b.addr), rd.register_start,
			     rd.registers_num, rd.fetch_index, r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
		}
	}
}


static void UpdateGraphicsDynamicState(const CommandBuffer& buffer, DynamicState& dynamic_state,
                                       const ShaderVertexInputInfo& vs_input_info,
                                       const RenderDepthInfo& depth, const RenderState& rendering) {

	KYTY_PROFILER_FUNCTION();

	const auto& ctx = buffer.GetRegisters();
	const auto&        vp  = ctx.GetScreenViewport();
	const vk::Extent2D framebuffer_extent {rendering.width, rendering.height};
	const auto& outputs = vs_input_info.stage.program->info.outputs;
	const bool  indexed_viewports =
	    std::any_of(outputs.begin(), outputs.end(), [](const auto& output) {
		    return output.kind == ShaderRecompiler::IR::StageOutputKind::ViewportIndex;
	    });
	constexpr uint32_t viewport_slots = std::size(HW::ScreenViewport {}.viewports);
	std::array<vk::Viewport, viewport_slots> viewports {};
	std::array<vk::Rect2D, viewport_slots>   scissors {};
	const uint32_t viewport_count = indexed_viewports ? viewport_slots : 1;
	for (uint32_t i = 0; i < viewport_count; i++) {
		const auto& guest    = vp.viewports[i];
		auto&       viewport = viewports[i];
		if (ctx.GetClipControl().clip_disable) {
			const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
			viewport.width  = static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u));
			viewport.height = static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u));
		} else {
			viewport.x      = guest.xoffset - guest.xscale;
			viewport.y      = guest.yoffset - guest.yscale;
			viewport.width  = guest.xscale * 2.0f;
			viewport.height = guest.yscale * 2.0f;
		}
		viewport.minDepth =
		    guest.zoffset - (ctx.GetClipControl().dx_clip_space ? 0.0f : guest.zscale);
		viewport.maxDepth = guest.zscale + guest.zoffset;

		const auto final_scissor =
		    calc_final_scissor(vp, ctx.GetScanModeControl(), framebuffer_extent, i);
		auto& scissor  = scissors[i];
		scissor.offset = {final_scissor.left, final_scissor.top};
		scissor.extent = {static_cast<uint32_t>(final_scissor.right - final_scissor.left),
		                  static_cast<uint32_t>(final_scissor.bottom - final_scissor.top)};
		if (viewport.width == 0.0f) {
			// Keep empty slots at their guest index; Vulkan requires a positive viewport width.
			viewport.width = 1.0f;
			scissor.extent = {0, 0};
		}
	}
	dynamic_state.SetViewports(std::span {viewports}.first(viewport_count));
	dynamic_state.SetScissors(std::span {scissors}.first(viewport_count));

	float line_width = ctx.GetLineWidth();
	if (line_width != 1.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("Render: temporary: clamping Vulkan line width %f to 1.0 because wideLines is "
			     "not enabled\n",
			     line_width);
			logged = true;
		}
		line_width = 1.0f;
	}
	dynamic_state.SetLineWidth(line_width);
	const auto&      blend = ctx.GetBlendColor();
	const std::array blend_constants {blend.red, blend.green, blend.blue, blend.alpha};
	dynamic_state.SetBlendConstants(blend_constants);
	dynamic_state.SetDepthTestEnabled(depth.depth_test_enable);
	dynamic_state.SetDepthWriteEnabled(depth.depth_write_enable);
	dynamic_state.SetDepthCompareOp(depth.depth_compare_op);
#if !defined(__APPLE__)
	dynamic_state.SetDepthBoundsTestEnabled(depth.depth_bounds_test_enable);
	if (depth.depth_bounds_test_enable) {
		dynamic_state.SetDepthBounds(ctx.GetDepthBoundsMin(), ctx.GetDepthBoundsMax());
	}
#endif

	const auto& mode              = ctx.GetModeControl();
	const auto& poly_offset       = ctx.GetPolyOffset();
	const bool  use_front         = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back          = mode.poly_offset_back_enable && !mode.cull_back;
	const bool  depth_bias_enable = use_front || use_back;
	dynamic_state.SetDepthBiasEnabled(depth_bias_enable);
	if (depth_bias_enable) {
		// Vulkan has one bias for both faces. Prefer a visible front face when both are enabled.
		const float guest_constant_factor =
		    use_front ? poly_offset.front_offset : poly_offset.back_offset;
		const float constant_factor = ConvertPolygonOffsetConstantFactor(
		    guest_constant_factor, poly_offset, depth.desc.view_info.format);
		const float slope_factor =
		    (use_front ? poly_offset.front_scale : poly_offset.back_scale) / 16.0f;
		dynamic_state.SetDepthBias(constant_factor, poly_offset.clamp, slope_factor);
	}

	dynamic_state.SetStencilTestEnabled(depth.stencil_test_enable);
	if (depth.stencil_test_enable) {
		const auto& front = depth.stencil_front;
		const auto& back  = depth.stencil_back;
		dynamic_state.SetStencilOps({front.failOp, front.passOp, front.depthFailOp, front.compareOp},
		                            {back.failOp, back.passOp, back.depthFailOp, back.compareOp});
		dynamic_state.SetStencilCompareMasks(front.compareMask, back.compareMask);
		dynamic_state.SetStencilWriteMasks(front.writeMask, back.writeMask);
		dynamic_state.SetStencilReferences(front.reference, back.reference);
	}

#if defined(__APPLE__)
	// MoltenVK has no VK_EXT_color_write_enable; the pipeline is created without the
	// eColorWriteEnableEXT dynamic state and relies on the static colorWriteMask instead.
#else

	vk::Bool32 enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t slot = 0; slot < rendering.num_color_attachments; slot++) {
		enable[slot] = rendering.color_attachments[slot].image_view != nullptr;
	}
	dynamic_state.SetColorWriteEnables(std::span {enable}.first(rendering.num_color_attachments));

#endif
}

static bool DrawHasValidVertexShader(const HW::Shader& sh_ctx) {

	const auto& vs = sh_ctx.GetVs();
	return vs.es_regs.data_addr != 0;
}

static bool PixelShaderHasDepthOrCoverageSideEffects(const HW::ShaderRegisters& sh_regs) {
	const auto& db = sh_regs.db_shader_control;
	return db.shader_kill_enable || db.shader_z_export_enable || db.shader_mask_export_enable ||
	       db.shader_dual_export_enable || db.shader_execute_on_noop || sh_regs.AlphaToMask();
}

struct DrawRenderState {
	RenderDepthInfo       depth_info;
	RenderColorInfo       color_info[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	uint32_t              color_count                              = 0;
	bool                  ps_active                                = true;
	std::array<ShaderVertexInputInfo, 3> vertex_info;
	ShaderPixelInputInfo  ps_input_info;
	PipelineCache::GraphicsPrograms programs;
};

struct DrawCallInfo {
	CommandBufferDebugOp debug_op       = CommandBufferDebugOp::DrawIndex;
	uint32_t             index_count    = 0;
	uint32_t             instance_count = 0;
	uint32_t             first_instance = 0;

	[[nodiscard]] bool IsIndexed() const { return debug_op == CommandBufferDebugOp::DrawIndex; }
	[[nodiscard]] const char* Name() const { return IsIndexed() ? "DrawIndex" : "DrawIndexAuto"; }
};

RenderState RenderExecutor::AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
                                                 uint32_t color_count, RenderDepthInfo& depth,
                                                 vk::ImageAspectFlags& feedback_aspects,
                                                 std::span<PreparedBindings* const> stages) {
	EXIT_IF(colors == nullptr || color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	feedback_aspects = {};
	auto&       cache = m_context.GetTextureCache();
	RenderState state {};
	state.width                 = std::numeric_limits<uint32_t>::max();
	state.height                = std::numeric_limits<uint32_t>::max();
	state.num_layers            = std::numeric_limits<uint32_t>::max();
	state.num_color_attachments = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		auto& target = colors[i];
		EXIT_IF(!target.image_id);
		const auto owner = cache.m_slot_images.try_get(target.image_id);
		if (owner == nullptr || (!owner->registered && !owner->info.data.Empty()) ||
		    owner->binding.needs_rebind) {
			EXIT("color target changed after render-state discovery\n");
		}
		const auto image_view = cache.FindRenderTarget(target.image_id, target.desc);
		auto&      image      = cache.GetImage(target.image_id);
		EXIT_IF(image.backing.samples != target.desc.info.samples || image_view == nullptr);
		const auto& view   = target.desc.view_info;
		const auto  layout = image.binding.is_bound ? vk::ImageLayout::eGeneral
		                                            : vk::ImageLayout::eColorAttachmentOptimal;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access =
		    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
		image.Transit(layout, image.binding.attachment_access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		const auto extent       = target.Extent();
		state.width             = std::min(state.width, extent.width);
		state.height            = std::min(state.height, extent.height);
		// Remember this color target for scene-target auto-detection.
		NoteSceneTargetWrite(target.desc.info.data.address, extent, target.image_id,
		                     target.desc.info.pixel_format);
		state.num_layers        = std::min(state.num_layers, view.layer_count);
		state.num_color_attachments = std::max(state.num_color_attachments, target.target_slot + 1);
		auto& attachment            = state.color_attachments[target.target_slot];
		attachment.image_view   = image_view;
		attachment.image_layout = layout;
	}
	if (depth.image_id) {
		const auto owner = cache.m_slot_images.try_get(depth.image_id);
		if (owner == nullptr || !owner->registered || owner->binding.needs_rebind) {
			EXIT("depth target changed after render-state discovery\n");
		}
		const auto  image_view = cache.FindDepthTarget(depth.image_id, depth.desc);
		const auto& metadata   = depth.desc.info.metadata;
		const bool meta_clear =
		    metadata.kind == ImageMetadataKind::Htile &&
		    cache.IsMetaCleared(metadata.range.address, depth.desc.view_info.base_layer);
		depth.load_clear_aspects = {};
		if (depth.depth_clear_enable || meta_clear)
			depth.load_clear_aspects |= vk::ImageAspectFlagBits::eDepth;
		// Apply pending clears to compressed stencil as well as depth.
		if (depth.stencil_clear_enable || (meta_clear && metadata.stencil_compressed))
			depth.load_clear_aspects |= vk::ImageAspectFlagBits::eStencil;
		if (meta_clear &&
		    !cache.TouchMeta(metadata.range.address, depth.desc.view_info.base_layer, false)) {
			EXIT("failed to consume HTile clear state\n");
		}
		auto& image = cache.GetImage(depth.image_id);
		EXIT_IF(image_view == nullptr || image.backing.samples != depth.desc.info.samples);
		const auto draw_writes = depth.AttachmentWriteAspects();
		vk::ImageAspectFlags sampled_aspects;
		for (const auto* stage: stages) {
			for (const auto& binding: stage->images) {
				if (binding.image_id != depth.image_id ||
				    binding.desc.type != TextureCache::BindingType::Texture) continue;
				const auto native =
				    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
				EXIT_IF(native == image.views.end());
				sampled_aspects |= native->info.aspect;
				feedback_aspects |= DepthFeedbackAspects(draw_writes, depth.desc.view_info,
				                                         native->info);
			}
		}
		if (feedback_aspects && !m_context.GetGraphics().attachment_feedback_loop_enabled) {
			// No host feedback-loop support (e.g. older AMD drivers): fall back to
			// General layout and continue. Sampling a depth attachment that is
			// also bound for writing is racy without the extension, so depth
			// reads in these draws may be stale; that beats aborting the game.
			static std::atomic_uint feedback_log = 0;
			if (feedback_log.fetch_add(1, std::memory_order_relaxed) < 4) {
				LOGF("Render: depth feedback loop unsupported by host; continuing "
				     "with General layout (aspects=0x%x)\n",
				     static_cast<uint32_t>(feedback_aspects));
			}
		}
		auto layout = depth_attachment_layout(depth);
		if (sampled_aspects & ~DepthReadableAspects(layout)) {
			layout = m_context.GetGraphics().attachment_feedback_loop_enabled
			             ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
			             : vk::ImageLayout::eGeneral;
		}
		// The attachment store writes even when guest depth/stencil tests do not.
		const auto access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
		                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access = access;
		const auto& view                = depth.desc.view_info;
		image.Transit(layout, access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		state.width               = std::min(state.width, depth.desc.info.extent.width);
		state.height              = std::min(state.height, depth.desc.info.extent.height);
		state.num_layers          = std::min(state.num_layers, view.layer_count);
		const auto aspects        = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		auto&      attachment     = state.depth_stencil_attachment;
		attachment.image_view     = image_view;
		attachment.image_layout   = layout;
		attachment.clear_value[0] = std::bit_cast<uint32_t>(depth.depth_clear_value);
		attachment.clear_value[1] = depth.stencil_clear_value;
		attachment.has_depth      = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.depth_clear =
		    static_cast<bool>(depth.load_clear_aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.has_stencil    = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eStencil);
		attachment.stencil_clear =
		    static_cast<bool>(depth.load_clear_aspects & vk::ImageAspectFlagBits::eStencil);
	}
	if (color_count == 0 && !depth.image_id) {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		state.width        = limits.maxFramebufferWidth;
		state.height       = limits.maxFramebufferHeight;
	}
	if (state.num_layers == std::numeric_limits<uint32_t>::max()) {
		state.num_layers = 1;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.width == std::numeric_limits<uint32_t>::max() ||
	        state.height == std::numeric_limits<uint32_t>::max());
	return state;
}

static uint32_t DrawColorOutputMask(const HW::Context& ctx) {
	const auto& sh_regs     = ctx.GetShaderRegisters();
	const auto  write_mask  = ctx.GetRenderTargetMask() & sh_regs.m_cbShaderMask;
	uint32_t    output_mask = ctx.HasObjectPrimitiveIdOutput() ? 1u << 7u : 0;
	for (uint32_t index = 0; index < RENDER_COLOR_ATTACHMENTS_MAX; index++) {
		const auto slot = ShaderPixelExportTarget(sh_regs.m_cbShaderMask, index);
		if (slot >= RENDER_COLOR_ATTACHMENTS_MAX) {
			break;
		}
		if (sh_regs.target_output_mode[index] != 0 &&
		    render_target_mask_slot(write_mask, slot) != 0) {
			output_mask |= 1u << slot;
		}
	}
	return output_mask;
}

enum class CbColorMode : uint8_t {
	Disable            = 0,
	Normal             = 1,
	EliminateFastClear = 2,
	Resolve            = 3,
	FmaskDecompress    = 5,
	DccDecompress      = 6,
};

bool RenderExecutor::ConsumeMetadataColorOperation(CommandBuffer& buffer,
                                                   uint32_t render_target_slice_offset) {
	const auto& ctx  = buffer.GetRegisters();
	const auto  mode = ctx.GetColorControl().mode;
	if (mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear)) {
		// FCE can precede any ordinary draw. Discover the target to materialize its native
		// clear metadata before the expanded image is sampled.
		RenderColorInfo target {};
		ResolveRenderColorTarget(buffer, target, render_target_slice_offset, 0, true);
		return true;
	}
	// Metadata/decompression shaders must not export colors into the expanded image.
	return mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ||
	       mode == static_cast<uint8_t>(CbColorMode::DccDecompress);
}

// vertex_offset is the first vertex of non-indexed draws.
struct DrawEmitInfo {
	int32_t          vertex_offset  = 0;
	uint32_t         first_instance = 0;
	DrawIndirectArgs indirect;
};

struct DrawIndexBufferSource {
	uint64_t      address   = 0;
	const void*   host_data = nullptr;
	uint64_t      size      = 0;
	vk::IndexType type      = vk::IndexType::eUint16;
	uint32_t      guest_element_size = 0;
};

struct PreparedIndexBuffer {
	vk::Buffer     buffer = nullptr;
	vk::DeviceSize offset = 0;
	vk::IndexType  type   = vk::IndexType::eUint16;
};

static uint64_t VertexBufferDescriptorSize(int binding, const ShaderVertexInputInfo& info) {
	const auto& buffer = info.buffers[binding];
	if (buffer.stride != 0 || buffer.num_records == 0) {
		return static_cast<uint64_t>(buffer.stride) * buffer.num_records;
	}

	uint64_t size = 0;
	for (int i = 0; i < info.resources_num; i++) {
		if (info.resources_dst[i].buffer_index != binding) {
			continue;
		}
		const auto& resource = info.resources[i];
		// RDNA2 OOB_SELECT=2 only checks NumRecords != 0. A constant attribute still
		// fetches its entire format; NumRecords is not a byte count in this mode.
		const uint64_t extent = resource.OutOfBounds() == 2
		                            ? resource.Base48() - buffer.addr +
		                                  ShaderRecompiler::Format::GetFormatInfo(resource.Format()).byte_size
		                            : buffer.num_records;
		size = std::max(size, extent);
	}
	return size;
}

struct VertexBufferRange {
	uint64_t                     base_address  = 0;
	uint64_t                     requested_end = 0;
	uint64_t                     acquired_end  = 0;
	std::pair<Buffer*, uint64_t> binding;

	[[nodiscard]] uint64_t RequestedSize() const { return requested_end - base_address; }
};

struct PreparedVertexBuffers {
	static constexpr uint32_t MaxBuffers = ShaderVertexInputInfo::RES_MAX;

	std::array<vk::Buffer, MaxBuffers>     buffers {};
	std::array<vk::DeviceSize, MaxBuffers> offsets {};
	std::array<vk::DeviceSize, MaxBuffers> sizes {};
	uint32_t                               count = 0;
};

static PreparedVertexBuffers AcquireVertexBuffers(CommandBuffer&               buffer,
                                                  const ShaderVertexInputInfo& vs_input_info) {
	EXIT_IF(vs_input_info.buffers_num < 0 ||
	        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX);

	// Collect the non-empty guest vertex ranges.
	std::array<uint64_t, ShaderVertexInputInfo::RES_MAX>          sizes {};
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> ranges {};
	uint32_t                                                      range_count = 0;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(i, vs_input_info);
		sizes[i]           = size;
		if (size == 0) {
			continue;
		}
		if (vertex.addr == 0 || size > UINT64_MAX - vertex.addr) {
			EXIT("invalid vertex buffer range: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vertex.addr, size);
		}
		ranges[range_count++] = {vertex.addr, vertex.addr + size};
	}

	std::sort(ranges.begin(), ranges.begin() + range_count,
	          [](const VertexBufferRange& left, const VertexBufferRange& right) {
		          return left.base_address < right.base_address;
	          });

	// Merge overlapping or touching ranges before acquiring host buffers.
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> merged_ranges {};
	uint32_t                                                      merged_count = 0;
	for (uint32_t i = 0; i < range_count; i++) {
		const auto& range = ranges[i];
		if (merged_count != 0 &&
		    merged_ranges[merged_count - 1].requested_end >= range.base_address) {
			merged_ranges[merged_count - 1].requested_end =
			    std::max(merged_ranges[merged_count - 1].requested_end, range.requested_end);
			continue;
		}
		merged_ranges[merged_count++] = {range.base_address, range.requested_end};
	}

	auto& cache = buffer.GetContext().GetBufferCache();
	for (uint32_t i = 0; i < merged_count; i++) {
		auto& range = merged_ranges[i];
		// PPSA20298
		const auto size =
		    Libs::LibKernel::Memory::ClampRangeSize(range.base_address, range.RequestedSize());
		range.acquired_end = range.base_address + size;
		range.binding      = cache.ObtainBuffer(range.base_address, size, false);
	}

	// Rebuild slot bindings, offsetting non-empty slots into their acquired merged range.
	PreparedVertexBuffers prepared;
	prepared.count         = static_cast<uint32_t>(vs_input_info.buffers_num);
	vk::Buffer null_buffer = nullptr;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = sizes[i];
		if (size == 0) {
			if (null_buffer == nullptr) {
				null_buffer = cache.GetBuffer(NULL_BUFFER_ID).Handle();
			}
			prepared.buffers[i] = null_buffer;
			prepared.offsets[i] = 0;
			continue;
		}

		const auto range = std::find_if(merged_ranges.begin(), merged_ranges.begin() + merged_count,
		                                [&](const VertexBufferRange& value) {
			                                return vertex.addr >= value.base_address &&
			                                       vertex.addr < value.acquired_end;
		                                });
		if (range == merged_ranges.begin() + merged_count) {
			EXIT("vertex buffer address is outside the acquired range: addr=0x%016" PRIx64 "\n",
			     vertex.addr);
		}

		prepared.buffers[i] = range->binding.first->Handle();
		prepared.offsets[i] = range->binding.second + vertex.addr - range->base_address;
		prepared.sizes[i]   = std::min(size, range->acquired_end - vertex.addr);
	}

	return prepared;
}

static void SetDrawDebugPhase(CommandBuffer& buffer, uint64_t submit_id, const DrawCallInfo& draw,
                              uint32_t phase) {
	buffer.SetDebugInfo(static_cast<uint32_t>(draw.debug_op), submit_id, phase, draw.index_count, 0,
	                    draw.instance_count, draw.first_instance);
}

static bool GetDrawTopology(const HW::UserConfig& ucfg, vk::PrimitiveTopology& topology) {

	topology = vk::PrimitiveTopology::ePointList;

	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kNone: return false;
		case Prospero::PrimitiveType::kPointList:
			topology = vk::PrimitiveTopology::ePointList;
			break;
		case Prospero::PrimitiveType::kLineList: topology = vk::PrimitiveTopology::eLineList; break;
		case Prospero::PrimitiveType::kLineStrip:
			topology = vk::PrimitiveTopology::eLineStrip;
			break;
		case Prospero::PrimitiveType::kTriList:
			topology = vk::PrimitiveTopology::eTriangleList;
			break;
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kPolygon:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			topology = vk::PrimitiveTopology::eTriangleStrip;
			break;
		case Prospero::PrimitiveType::kPatch:
			if (!Config::TessellationEnabled()) return false;
			[[fallthrough]];
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
			topology = vk::PrimitiveTopology::ePatchList;
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		default: {
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				std::printf("Skipping draw with unknown primitive type: %u\n",
				            static_cast<uint32_t>(ucfg.GetPrimType()));
			}
			return false;
		}
	}

	return true;
}

static bool ResolvePrimitiveRestart(const CommandBuffer& buffer,
                                    const DrawIndexBufferSource& source) {
	const auto control = buffer.GetUserConfig().GetPrimitiveResetControl();
	EXIT_NOT_IMPLEMENTED((control & ~0x3u) != 0);
	if ((control & 0x1u) == 0) {
		return false;
	}
	switch (buffer.GetUserConfig().GetPrimType()) {
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip: break;
		default: return false;
	}

	const auto element_size = source.guest_element_size;
	const auto index_mask   = UINT32_MAX >> ((4 - element_size) * 8);
	const auto reset_index  = buffer.GetRegisters().GetPrimitiveResetIndex();
	if ((control & 0x2u) != 0 && (reset_index & ~index_mask) != 0) {
		return false;
	}
	const auto restart_index = reset_index & index_mask;
	if (restart_index == index_mask) {
		// Use native restart; the 8-bit path widens its marker to 0xffff.
		return true;
	}

	// A game can set a custom reset value without using it in the index buffer.
	// Keep restart off in that case; fail if we actually find the value.
	// Scan before preparing draw resources: readback can restart the command buffer.
	EXIT_NOT_IMPLEMENTED(source.address == 0);
	const auto* indices = reinterpret_cast<const uint8_t*>(source.address);
	for (uint64_t offset = 0; offset < source.size; offset += element_size) {
		uint32_t index = 0;
		std::memcpy(&index, indices + offset, element_size);
		EXIT_NOT_IMPLEMENTED(index == restart_index);
	}
	return false;
}

static void RefreshShaders(CommandBuffer& buffer, const DrawCallInfo& draw,
                           uint32_t color_output_mask, DrawRenderState& state) {
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	const auto& vertex_shader_info = sh_ctx.GetVs();
	const auto& pixel_shader_info  = sh_ctx.GetPs();
	const auto& shader_regs        = ctx.GetShaderRegisters();

	state.programs      = {};
	state.ps_input_info = {};
	std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX>
	    target_export_mapping {};
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		const auto& rt = ctx.GetRenderTarget(slot);
		if ((color_output_mask & (1u << slot)) != 0 && rt.base.addr != 0) {
			target_export_mapping[slot] =
			    TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type,
			                                 rt.info.channel_order)
			        .export_mapping;
		}
	}
	auto& pipeline_cache = buffer.GetContext().GetPipelineCache();
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "GetGraphicsPrograms");
	}
	state.programs = pipeline_cache.GetGraphicsPrograms(
	    vertex_shader_info, pixel_shader_info, shader_regs, ctx, buffer.GetUserConfig(),
	    target_export_mapping, state.ps_active, state.vertex_info, state.ps_input_info);
}

bool RenderExecutor::PrepareDrawRenderState(CommandBuffer& buffer, const DrawCallInfo& draw,
                                            uint32_t            render_target_slice_offset,
	                                        DrawRenderState& state) {
	const auto& shader_regs       = buffer.GetRegisters().GetShaderRegisters();
	const auto  color_output_mask = DrawColorOutputMask(buffer.GetRegisters());
	state.ps_active = buffer.GetShaders().GetPs().ps_regs.data_addr != 0 &&
	                  (color_output_mask != 0 ||
	                   PixelShaderHasDepthOrCoverageSideEffects(shader_regs));
	RefreshShaders(buffer, draw, color_output_mask, state);
	if (state.programs.skip_draw) {
		// Unsupported shader/pipeline (logged at the skip site); the game
		// continues without this draw. Callers count this via census_state_fail.
		return false;
	}
	uint32_t mrt_mask = 0;
	if (state.ps_active) {
		for (const auto& output: state.ps_input_info.stage.program->info.outputs) {
			if (output.kind == ShaderRecompiler::IR::StageOutputKind::Mrt ||
			    output.kind == ShaderRecompiler::IR::StageOutputKind::ObjectPrimitiveId) {
				mrt_mask |= 1u << output.location;
			}
		}
	}
	mrt_mask &= color_output_mask;
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "ResolveRenderColorTarget");
	}
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if ((mrt_mask & (1u << slot)) != 0) {
			ResolveRenderColorTarget(buffer, state.color_info[state.color_count],
			                         render_target_slice_offset, slot);
			if (state.color_info[state.color_count].image_id) {
				state.color_count++;
			}
		}
	}
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "ResolveRenderDepthTarget");
	}
	if (!ResolveRenderDepthTarget(buffer, state.depth_info)) {
		// Unrepresentable depth/stencil state (logged at the skip site).
		return false;
	}
	// TODO(PPSA09243): Vulkan passes take one sample count. Where PS5 attachments mix counts,
	// keep those matching the raster count instead of resolving multisampled shadows.
	const auto colors       = std::span {state.color_info, state.color_count};
	const auto pass_samples = state.depth_info.image_id ? state.depth_info.desc.info.samples
	                                                    : state.color_info[0].desc.info.samples;
	if (std::ranges::any_of(colors, [&](const RenderColorInfo& color) {
		    return color.desc.info.samples != pass_samples;
	    })) {
		const auto raster =
		    render_sample_count(buffer.GetRegisters().GetAaConfig().msaa_num_samples);
		const auto dropped = std::ranges::remove_if(colors, [&](const RenderColorInfo& color) {
			return color.desc.info.samples != raster;
		});
		state.color_count -= static_cast<uint32_t>(dropped.size());
		if (state.depth_info.image_id && state.depth_info.desc.info.samples != raster) {
			state.depth_info = {};
		}
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			Log::WriteToConsoleAndLog(
			    "Warning: dropping attachments whose sample count differs from the raster.\n");
		}
	}

	if (state.color_count == 0 && !state.depth_info.image_id && !state.ps_active) {
		LogFramebufferSkip(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   draw.index_count, 0);
		return false;
	}

	return true;
}

static PreparedIndexBuffer PrepareIndexBuffer(CommandBuffer&               buffer,
                                              const DrawIndexBufferSource& source) {
	PreparedIndexBuffer prepared;
	if (source.size == 0) {
		return prepared;
	}
	prepared.type = source.type;
	if (source.host_data != nullptr) {
		auto& stream = buffer.GetContext().GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		prepared.offset = stream.Copy(source.host_data, source.size, 16);
		prepared.buffer = stream.Handle();
	} else {
		auto [buffer_ptr, offset] =
		    buffer.GetContext().GetBufferCache().ObtainBuffer(source.address, source.size, false);
		prepared.buffer = buffer_ptr->Handle();
		prepared.offset = offset;
	}
	return prepared;
}

static void CommitVertexBuffers(vk::CommandBuffer            vk_buffer,
                                const PreparedVertexBuffers& prepared) {
	for (uint32_t i = 0; i < prepared.count; i++) {
		EXIT_IF(prepared.buffers[i] == nullptr);
	}
	if (prepared.count != 0) {
		// Guest descriptor bounds must survive allocation merging in the cache.
		vk_buffer.bindVertexBuffers2(0, prepared.count, prepared.buffers.data(),
		                             prepared.offsets.data(), prepared.sizes.data(), nullptr);
	}
}

static void CommitIndexBuffer(vk::CommandBuffer vk_buffer, const PreparedIndexBuffer& prepared) {
	if (prepared.buffer == nullptr) {
		return;
	}
	vk_buffer.bindIndexBuffer(prepared.buffer, prepared.offset, prepared.type);
}

static void LogDrawStateIfNeeded(const CommandBuffer& buffer, const DrawCallInfo& draw,
	                             const DrawRenderState& state, uint32_t index_type_and_size,
                                 const void* index_addr) {
	// The full debug dump is extremely heavy (it also logs every CP packet), so
	// the draw-state line has its own switch: env KYTY_DRAW_STATE_SAMPLE=1 logs
	// one sampled line throughout the run without the rest of the dump.
	static const bool sample_enabled = std::getenv("KYTY_DRAW_STATE_SAMPLE") != nullptr;
	if (!graphics_debug_dump_enabled() && !sample_enabled) {
		return;
	}

	// Watch draws into the composite's scene source. This finds the tonemap
	// pass (which should fill it) and shows what it samples.
	const uint64_t watch_addr = g_composite_source_addr.load(std::memory_order_relaxed);
	if (watch_addr != 0 && state.color_count > 0 &&
	    state.color_info[0].desc.info.data.address == watch_addr) {
		static std::atomic_uint watch_logged {0};
		if (watch_logged.fetch_add(1, std::memory_order_relaxed) < 160) {
			LOGF("WatchDraw: frame=%d %s idx=%u indexed=%d ps=%d colors=%u depth=%d fmt=%s\n",
			     buffer.GetContext().GetGpu().GetFrameNum(), draw.Name(), draw.index_count,
			     draw.IsIndexed() ? 1 : 0, state.ps_active ? 1 : 0, state.color_count,
			     state.depth_info.image_id ? 1 : 0,
			     vk::to_string(state.color_info[0].desc.info.pixel_format).c_str());
		}
	}

	// Watch draws into the tonemap's exposure/LUT inputs (any extent).
	if (state.color_count > 0) {
		const uint64_t aux_addr = state.color_info[0].desc.info.data.address;
		const uint64_t ex_addr  = KytySceneDiag::exposure_addr.load(std::memory_order_relaxed);
		const uint64_t lu_addr  = KytySceneDiag::lut_addr.load(std::memory_order_relaxed);
		if (aux_addr != 0 && (aux_addr == ex_addr || aux_addr == lu_addr)) {
			static std::atomic_uint aux_logged {0};
			if (aux_logged.fetch_add(1, std::memory_order_relaxed) < 40) {
				LOGF("AuxDraw: frame=%d %s idx=%u ps=%d addr=0x%010" PRIx64 " ext=%ux%u\n",
				     buffer.GetContext().GetGpu().GetFrameNum(), draw.Name(),
				     draw.index_count, state.ps_active ? 1 : 0, aux_addr,
				     state.color_info[0].desc.info.extent.width,
				     state.color_info[0].desc.info.extent.height);
			}
		}
	}

	if (!draw.IsIndexed() && !Prospero::IsRectList(buffer.GetUserConfig().GetPrimType())) {
		return;
	}

	if (state.ps_active) {
		LogDrawTargetState(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   state.ps_input_info, draw.index_count, 0);
	}
	LogDrawInputState(buffer, state.color_info[0], state.vertex_info[0], index_type_and_size,
	                  draw.index_count, index_addr);
}

namespace {

// AMD Windows drivers reject valid barycentric fragment pipelines with triangle
// strips (ErrorUnknown, validation silent) while the same shaders work as lists.
// Expand strips/fans to lists for barycentric draws on AMD hardware. Winding flips
// preserve facing; the odd-triangle order preserves the provoking vertex convention.
constexpr uint32_t kAmdVendorId = 0x1002u;

bool PixelShaderUsesBarycentrics(const DrawRenderState& state) {
	if (!state.ps_active) {
		return false;
	}
	const auto* program = state.ps_input_info.stage.program;
	if (program == nullptr) {
		return false;
	}
	using Kind = ShaderRecompiler::IR::StageInputKind;
	for (const auto& input: program->info.inputs) {
		if (input.kind == Kind::BaryCoordSmooth || input.kind == Kind::BaryCoordSmoothCentroid ||
		    input.kind == Kind::BaryCoordNoPerspective) {
			return true;
		}
	}
	return false;
}

// Expands a strip/fan index sequence into a triangle list. Returns false when the
// draw does not need conversion or its indices cannot be read.
bool ExpandStripDrawToList(std::vector<uint32_t>& out, CommandBuffer& buffer,
                           vk::PrimitiveTopology topology, const DrawCallInfo& draw,
                           const DrawEmitInfo& emit, const DrawIndexBufferSource& source,
                           bool primitive_restart_enable, bool provoking_vtx_last) {
	if (topology != vk::PrimitiveTopology::eTriangleStrip &&
	    topology != vk::PrimitiveTopology::eTriangleFan) {
		return false;
	}
	const bool indexed = draw.IsIndexed();
	const uint32_t count = draw.index_count;
	if (count < 3u || count > 0x400000u) {
		return false;
	}
	const bool is_strip = topology == vk::PrimitiveTopology::eTriangleStrip;

	std::vector<uint32_t> src;
	src.reserve(count);
	if (indexed) {
		const uint32_t elem_bytes = (source.type == vk::IndexType::eUint32) ? 4u : 2u;
		if (source.host_data != nullptr) {
			const auto* bytes = static_cast<const uint8_t*>(source.host_data);
			for (uint32_t i = 0; i < count; i++) {
				uint32_t v = 0;
				std::memcpy(&v, bytes + static_cast<size_t>(i) * elem_bytes, elem_bytes);
				src.push_back(v);
			}
		} else {
			const uint64_t need = static_cast<uint64_t>(count) * elem_bytes;
			if (source.size < need) {
				return false;
			}
			std::vector<uint8_t> staging(static_cast<size_t>(need));
			// Sync GPU-written index data into CPU backing before expanding.
			if (!Libs::LibKernel::Memory::TryReadBufferBacking(source.address, staging.data(),
			                                                   staging.size())) {
				return false;
			}
			for (uint32_t i = 0; i < count; i++) {
				uint32_t v = 0;
				std::memcpy(&v, staging.data() + static_cast<size_t>(i) * elem_bytes, elem_bytes);
				src.push_back(v);
			}
		}
	} else {
		if (emit.vertex_offset > UINT32_MAX - count) {
			return false;
		}
		for (uint32_t i = 0; i < count; i++) {
			src.push_back(emit.vertex_offset + i);
		}
	}

	const bool     use_restart = primitive_restart_enable && indexed;
	const uint32_t restart =
	    (source.type == vk::IndexType::eUint32) ? 0xffffffffu : 0xffffu;
	out.clear();
	if (!is_strip) {
		for (uint32_t i = 2; i < count; i++) {
			const uint32_t a = src[0], b = src[i - 1], c = src[i];
			if (use_restart && (a == restart || b == restart || c == restart)) {
				continue;
			}
			out.push_back(a);
			out.push_back(b);
			out.push_back(c);
		}
		return !out.empty();
	}
	uint32_t w0 = 0, w1 = 0, have = 0, pos = 0;
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t v = src[i];
		if (use_restart && v == restart) {
			have = 0;
			pos  = 0;
			continue;
		}
		if (have < 2) {
			if (have == 0) {
				w0 = v;
			} else {
				w1 = v;
			}
			have++;
			continue;
		}
		if ((pos & 1u) == 0u) {
			out.push_back(w0);
			out.push_back(w1);
			out.push_back(v);
		} else if (provoking_vtx_last) {
			out.push_back(w1);
			out.push_back(w0);
			out.push_back(v);
		} else {
			out.push_back(w0);
			out.push_back(v);
			out.push_back(w1);
		}
		w0 = w1;
		w1 = v;
		pos++;
	}
	return !out.empty();
}

} // namespace

static void EmitDrawPrimitives(const HW::UserConfig& ucfg, vk::CommandBuffer vk_buffer,
                               const DrawCallInfo& draw, const DrawEmitInfo& emit) {
	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kPolygon:
		case Prospero::PrimitiveType::kRectListLegacy:
		case Prospero::PrimitiveType::kPatch:
			if (draw.IsIndexed()) {
				vk_buffer.drawIndexed(draw.index_count, draw.instance_count, 0, emit.vertex_offset,
				                      emit.first_instance);
			} else {
				vk_buffer.draw(draw.index_count, draw.instance_count,
				               static_cast<uint32_t>(emit.vertex_offset), emit.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				if (draw.IsIndexed()) {
					vk_buffer.drawIndexed(4, draw.instance_count, i, emit.vertex_offset,
					                      emit.first_instance);
				} else {
					vk_buffer.draw(4, draw.instance_count,
					               i + static_cast<uint32_t>(emit.vertex_offset),
					               emit.first_instance);
				}
			}
			break;
		default: EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(ucfg.GetPrimType()));
	}
}

bool RenderExecutor::CopyDepthToColor(CommandBuffer& buffer, const DrawCallInfo& draw,
                                       const DrawEmitInfo& emit, DrawRenderState& state) {
	if (!buffer.GetRegisters().GetRenderControl().copy_depth_to_color) {
		return false;
	}
	auto& ucfg = buffer.GetUserConfig();
	const auto& hw = buffer.GetRegisters();
	auto& depth = state.depth_info;
	auto& color = state.color_info[0];
	EXIT_NOT_IMPLEMENTED(!m_context.GetGraphics().maintenance8_enabled ||
	                     !Prospero::IsRectList(ucfg.GetPrimType()) ||
	                     draw.IsIndexed() || draw.index_count != 3 || draw.instance_count != 1 ||
	                     emit.vertex_offset != 0 || emit.first_instance != 0 ||
	                     state.color_count != 1 || color.target_slot != 0 || !depth.image_id ||
	                     color.desc.info.samples != 1 ||
	                     depth.desc.info.extent.width != color.Extent().width ||
	                     depth.desc.info.extent.height != color.Extent().height ||
	                     depth.desc.view_info.layer_count != 1 ||
	                     color.desc.view_info.layer_count != 1 ||
	                     hw.GetBlendControl(0).enable || state.ps_input_info.ps_pixel_kill_enable ||
	                     state.ps_input_info.ps_depth_export_enable ||
	                     state.ps_input_info.ps_sample_mask_export_enable);
	vk::ImageAspectFlags feedback;
	const auto rendering = AcquireRenderTargets(buffer, &color, 1, depth, feedback);
	if (depth.load_clear_aspects) {
		m_context.GetCommandScheduler().BeginRendering(rendering);
	}
	auto& cache = m_context.GetTextureCache();
	auto& source = cache.GetImage(depth.image_id);
	auto& destination = cache.GetImage(color.image_id);
	EXIT_NOT_IMPLEMENTED((source.backing.format != vk::Format::eD32Sfloat &&
	                      source.backing.format != vk::Format::eD32SfloatS8Uint) ||
	                     destination.backing.format != vk::Format::eR32Sfloat);
	const auto rect = calc_final_scissor(hw.GetScreenViewport(), hw.GetScanModeControl(),
	                                    {rendering.width, rendering.height}, 0);
	if (rect.right > rect.left && rect.bottom > rect.top) {
		const vk::ImageCopy region {
		    {vk::ImageAspectFlagBits::eDepth, depth.desc.view_info.base_level,
		     depth.desc.view_info.base_layer, 1}, {rect.left, rect.top, 0},
		    {vk::ImageAspectFlagBits::eColor, color.desc.view_info.base_level,
		     color.desc.view_info.base_layer, 1}, {rect.left, rect.top, 0},
		    {static_cast<uint32_t>(rect.right - rect.left),
		     static_cast<uint32_t>(rect.bottom - rect.top), 1}};
		m_context.GetCommandScheduler().EndRendering();
		const auto command = buffer.Handle();
		source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
		               {}, command);
		destination.Transit(vk::ImageLayout::eTransferDstOptimal,
		                    vk::AccessFlagBits2::eTransferWrite, {}, command);
		command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal,
		                  destination.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                  1, &region);
	}
	return true;
}

// Composite override: force the full-scene color target as texture[0] for draws
// into the video-out buffer so the rendered 3D world reaches the screen instead
// of a partial/wrong texture. The scene target address is taken from env
// KYTY_COMPOSITE_OVERRIDE_FILE (default composite_override.txt), or auto-detected
// as the large color render target with the most writes. Called after
// PrepareGraphicsBindings resolved the guest bindings but before CommitBindings,
// so the override survives to the draw.
static uint64_t CompositeOverrideAddress(uint64_t            exclude_address,
                                         const vk::Extent2D& want_extent) {
	const char* path = std::getenv("KYTY_COMPOSITE_OVERRIDE_FILE");
	if (path == nullptr || path[0] == '\0') {
		path = "composite_override.txt";
	}
	uint64_t file_addr = 0ull;
	if (FILE* file = std::fopen(path, "rb"); file != nullptr) {
		char buf[64] = {};
		if (std::fread(buf, 1, sizeof(buf) - 1, file) > 0) {
			std::fclose(file);
			file_addr = std::strtoull(buf, nullptr, 16);
		} else {
			std::fclose(file);
		}
	}
	if (file_addr != 0) {
		return file_addr;
	}
	// Fall back to the freshest large scene target, preferring an HDR colour
	// buffer that matches the composite source's resolution. The composite's
	// own (often unwritten) LDR source is excluded.
	return PickSceneTarget(exclude_address, want_extent).address;
}

static void ApplyCompositeOverride(CommandBuffer& buffer,
                                   const DrawRenderState& state,
                                   const DrawCallInfo& draw,
                                   PreparedBindings* pixel_bindings) {
	// Switch for checking whether the substitute is still needed at all:
	// KYTY_COMPOSITE_OVERRIDE=off leaves the guest's own composite untouched.
	static const bool disabled = [] {
		const char* value = std::getenv("KYTY_COMPOSITE_OVERRIDE");
		if (value == nullptr) {
			return false;
		}
		const std::string text {value};
		return text == "off" || text == "0" || text == "none";
	}();
	if (disabled) {
		return;
	}
	if (state.color_count == 0 || !state.color_info[0].image_id ||
	    pixel_bindings == nullptr || pixel_bindings->images.empty()) {
		return;
	}
	const uint64_t target_addr = state.color_info[0].desc.info.data.address;
	// Only composite draws target the video-out buffer (guest addr 0x8fc...).
	if (!IsVideoOutAddress(target_addr)) {
		return;
	}
	const auto guest_frame = static_cast<int64_t>(buffer.GetContext().GetGpu().GetFrameNum());
	// Each guest frame starts by drawing into the video-out buffer again; anything the
	// previous frame left there (including whether it came from the linear scene target) is
	// replaced now. Doing this here, instead of by elapsed frames in the presenter, keeps
	// the answer tied to the pixels: a menu or loading screen clears it on its own first
	// draw and is therefore never display-encoded.
	ClearVideoOutLinearBeforeFrame(target_addr, guest_frame);
	auto&          tb        = pixel_bindings->images[0];
	const uint64_t bound_addr = tb.desc.info.data.address;
	const uint32_t bound_w    = tb.desc.info.extent.width;
	const uint32_t bound_h    = tb.desc.info.extent.height;
	const auto     bound_extent = vk::Extent2D {bound_w, bound_h};
	const uint64_t ov_addr    = CompositeOverrideAddress(bound_addr, bound_extent);

	// The world composite samples an LDR scene-sized source (3360x1892 here);
	// the HUD/UI composite is also a fullscreen quad into the same buffer but
	// samples a 1792x1080 UI texture, and HUD elements are smaller still.
	// Requiring a scene-sized source keeps the UI composite on its own texture.
	const bool scene_sized = bound_w >= 2560u && bound_h >= 1400u;
	if (scene_sized && draw.index_count <= 6u) {
		// Remember the scene source for the draw-watch diagnostic. The world
		// composite runs after the tonemap pass, so this value is available
		// when the next frame's tonemap draw executes.
		g_composite_source_addr.store(bound_addr, std::memory_order_relaxed);
	}

	// Diagnosis: log fullscreen quads and what they sample.
	static std::atomic_uint vo_logged {0};
	if ((draw.index_count <= 6u || scene_sized) &&
	    vo_logged.fetch_add(1, std::memory_order_relaxed) < 400) {
		LOGF("VoFullscreen: frame=%d target=0x%010" PRIx64 " indices=%u tex0=0x%010" PRIx64
		     " tex0_ext=%ux%u tex0_fmt=%u scene=%d ov=0x%010" PRIx64 "\n",
		     buffer.GetContext().GetGpu().GetFrameNum(), target_addr, draw.index_count,
		     bound_addr, bound_w, bound_h,
		     static_cast<uint32_t>(tb.desc.info.pixel_format), scene_sized ? 1 : 0, ov_addr);
	}

	if (draw.index_count > 6u || !scene_sized || IsVideoOutAddress(bound_addr)) {
		static const bool diag_draws = std::getenv("KYTY_PRESENT_DIAG") != nullptr;
		static std::atomic_uint vo_diag {0};
		if (diag_draws && vo_diag.fetch_add(1, std::memory_order_relaxed) < 600) {
			std::printf("VoDraw: frame=%d target=0x%010" PRIx64 " idx=%u bound=0x%010" PRIx64
			            " ext=%ux%u scene=%d\n",
			            buffer.GetContext().GetGpu().GetFrameNum(), target_addr, draw.index_count,
			            bound_addr, bound_w, bound_h, scene_sized ? 1 : 0);
			std::fflush(stdout);
		}
		return;
	}
	auto& cache = buffer.GetContext().GetTextureCache();

	// Does the composite source address itself have a render-target image with
	// a different id? That would mean the sampled texture is a stale alias and
	// the correctly tonemapped LDR image exists separately.
	const auto ldr_stats = SceneTargetStatsFor(bound_addr);
	const auto pick      = PickSceneTarget(bound_addr, bound_extent);
	const auto guest_mapping = tb.desc.view_info.mapping;
	static std::atomic_uint pick_logged {0};
	const unsigned          pick_n = pick_logged.fetch_add(1, std::memory_order_relaxed);
	if (pick_n < 8 || (pick_n % 500u) == 0) {
		LOGF("VoPick: frame=%d bound=0x%010" PRIx64 " id=%u/%u tracked_id=%u/%u"
		     " tracked_fmt=%s tracked_draws=%u pick=0x%010" PRIx64 " pick_id=%u/%u"
		     " pick_fmt=%s pick_seq=%" PRIu64 " map=%d,%d,%d,%d images=%zu\n",
		     buffer.GetContext().GetGpu().GetFrameNum(), bound_addr, tb.image_id.index,
		     tb.image_id.generation,
		     ldr_stats.draws != 0 ? ldr_stats.image_id.index : 0u,
		     ldr_stats.draws != 0 ? ldr_stats.image_id.generation : 0u,
		     vk::to_string(ldr_stats.format).c_str(), ldr_stats.draws, pick.address,
		     pick.image_id.index, pick.image_id.generation,
		     vk::to_string(pick.format).c_str(), pick.last_seq,
		     static_cast<int>(guest_mapping.r), static_cast<int>(guest_mapping.g),
		     static_cast<int>(guest_mapping.b), static_cast<int>(guest_mapping.a),
		     pixel_bindings->images.size());
		for (size_t slot = 0; slot < pixel_bindings->images.size() && slot < 8; slot++) {
			const auto& b = pixel_bindings->images[slot];
			LOGF("VoPickImg[%zu]: addr=0x%010" PRIx64 " id=%u/%u ext=%ux%u fmt=%s\n", slot,
			     b.desc.info.data.address, b.image_id.index, b.image_id.generation,
			     b.desc.info.extent.width, b.desc.info.extent.height,
			     vk::to_string(b.desc.info.pixel_format).c_str());
		}
	}

	uint64_t use_addr = ov_addr;
	if (use_addr == bound_addr) {
		// Never sample the composite's own (usually empty) source back into it.
		use_addr = pick.address;
	}
	ImageId id {};
	if (std::getenv("KYTY_USE_LDR_TARGET") != nullptr && ldr_stats.image_id &&
	    ldr_stats.image_id != tb.image_id) {
		// Experiment: the tracked render-target image may hold the real
		// tonemapped LDR frame while the sampled alias is empty.
		id = ldr_stats.image_id;
	} else if (use_addr != 0) {
		id = cache.FindImageFromRange(use_addr, 4, false);
	}
	if (!id) {
		// The substitute could not be resolved, so this composite draws the guest's own
		// source into the video-out buffer; its contents are the guest's, not ours.
		SetVideoOutLinearFrame(target_addr, 0);
		return;
	}
	auto& image = cache.GetImage(id);
	TextureCache::ImageDesc scene_desc {};
	scene_desc.info = image.info;
	scene_desc.view_info.format = image.info.pixel_format;
	scene_desc.view_info.type = vk::ImageViewType::e2D;
	scene_desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
	scene_desc.view_info.base_level = 0;
	scene_desc.view_info.level_count = 1;
	scene_desc.view_info.base_layer = 0;
	scene_desc.view_info.layer_count = 1;
	scene_desc.view_info.usage = vk::ImageUsageFlagBits::eSampled;
	// Scene targets are rendered by the guest's own shaders: their channel
	// order is already the logical RGB the composite expects, so an identity
	// view is correct here. The old texture's swizzle describes its storage,
	// not the scene buffer's.
	scene_desc.type = TextureCache::BindingType::Texture;
	tb.image_id = id;
	tb.image_view = cache.FindTexture(id, scene_desc);
	tb.desc = scene_desc;
	// The presenter must display-encode this buffer's contents for this frame.
	g_composite_override_tick.fetch_add(1, std::memory_order_relaxed);
	SetVideoOutLinearFrame(target_addr, buffer.GetContext().GetGpu().GetFrameNum());
	static const bool diag = std::getenv("KYTY_PRESENT_DIAG") != nullptr;
	static std::atomic_uint logged {0};
	if (diag && logged.fetch_add(1, std::memory_order_relaxed) < 4000) {
		std::printf("OvFmt: seq=%llu frame=%d target=0x%010" PRIx64 " fmt=%s ext=%ux%u addr=0x%010" PRIx64 "\n",
		            static_cast<unsigned long long>(
		                g_composite_override_tick.load(std::memory_order_relaxed)),
		            buffer.GetContext().GetGpu().GetFrameNum(), target_addr,
		            vk::to_string(image.info.pixel_format).c_str(), image.info.extent.width,
		            image.info.extent.height, image.info.data.address);
		std::fflush(stdout);
	}
	if (logged.fetch_add(1, std::memory_order_relaxed) < 40) {
		LOGF("CompositeOverride: tex0 -> 0x%010" PRIx64 " id=%u/%u ext=%ux%u fmt=%s frame=%d\n",
		     image.info.data.address, id.index, id.generation, image.info.extent.width,
		     image.info.extent.height, vk::to_string(image.info.pixel_format).c_str(),
		     buffer.GetContext().GetGpu().GetFrameNum());
	}
}

void RenderExecutor::ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer,
                                         const DrawCallInfo& draw, DrawRenderState& state,
                                         vk::PrimitiveTopology topology, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source,
	                                     bool primitive_restart_enable) {
	// TEMP-DIAG: insane counts hang the GPU (TDR) with no other trace.
	if (draw.index_count > 1000000u || draw.instance_count > 4096u) {
		std::printf("SUSPICIOUS DRAW: submit=%llu indexed=%d index_count=%u instances=%u topo=%u\n",
		            static_cast<unsigned long long>(submit_id), draw.IsIndexed() ? 1 : 0,
		            draw.index_count, draw.instance_count, static_cast<uint32_t>(topology));
		std::fflush(stdout);
	}
	if (state.programs.skip_draw) {
		g_census_mesh_skip.fetch_add(1, std::memory_order_relaxed);
		ResetBindings();
		return;
	}
	auto& ucfg = buffer.GetUserConfig();
	if (CopyDepthToColor(buffer, draw, emit, state)) {
		return;
	}
	const auto vertex_stages =
	    std::span {state.vertex_info.data(), state.programs.VertexStageCount()};
	const bool mesh_active = state.vertex_info[0].stage.program->stage == ShaderType::Mesh;
	uint32_t   mesh_groups = 0;
	if (mesh_active) {
		const auto& mesh = state.vertex_info[0].mesh;
		EXIT_NOT_IMPLEMENTED(mesh.fast_launch && (draw.IsIndexed() || primitive_restart_enable));
		static std::atomic_bool restart_warned = false;
		if (primitive_restart_enable && !restart_warned.exchange(true, std::memory_order_relaxed)) {
			std::printf("Warning: primitive restart is not implemented for mesh shaders; "
			            "continuing draw (primitive=%u indexed=%u)\n",
			            static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed());
		}
		if (mesh.primitives_per_group == 0) {
			EXIT("unsupported mesh draw: primitive=%u indexed=%u restart=%u\n",
			     static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed(), primitive_restart_enable);
		}
		const auto primitives = mesh.InputPrimitiveCount(draw.index_count);
		if (primitives == 0 || draw.instance_count == 0) {
			return;
		}
		mesh_groups        = (primitives - 1u) / mesh.primitives_per_group + 1u;
		const auto& limits = m_context.GetGraphics().mesh_shader_properties;
		if (mesh_groups > limits.maxMeshWorkGroupCount[0] ||
		    draw.instance_count > limits.maxMeshWorkGroupCount[1] ||
		    static_cast<uint64_t>(mesh_groups) * draw.instance_count >
		        limits.maxMeshWorkGroupTotalCount) {
			EXIT("mesh draw exceeds host workgroup limits: %ux%u\n", mesh_groups,
			     draw.instance_count);
		}
	}

	if (mesh_active && draw.IsIndexed()) {
		// Register the original guest indices for shader reads; PrepareGraphicsBindings
		// synchronizes registered BDA ranges before any draw commands are committed.
		(void)m_context.GetBufferCache().FindBuffer(
		    index_source.address, static_cast<uint64_t>(draw.index_count) *
		                              index_source.guest_element_size);
	}
	LogDrawPhase(draw.Name(), "PrepareBindings");
	auto&                            bindings = m_graphics_bindings;
	std::array<PreparedBindings*, 4> descriptor_stages {};
	uint32_t                         stage_count = 0;
	for (uint32_t i = 0; i < vertex_stages.size(); i++) {
		PrepareBindings(state.vertex_info[i].stage, bindings.vertex[i]);
		descriptor_stages[stage_count++] = &bindings.vertex[i];
	}
	if (state.ps_active) {
		if (!bindings.pixel) bindings.pixel.emplace();
		PrepareBindings(state.ps_input_info.stage, *bindings.pixel);
		descriptor_stages[stage_count++] = &*bindings.pixel;
	}
	const auto stages = std::span {descriptor_stages.data(), stage_count};
	PrepareGraphicsBindings(stages, std::span {state.color_info, state.color_count});
	// Composite override: present the rendered 3D world instead of a wrong/black
	// texture bound by the guest for the video-out composite pass.
	ApplyCompositeOverride(buffer, state, draw,
	                       m_graphics_bindings.pixel ? &*m_graphics_bindings.pixel : nullptr);

	// Diagnose the pass that fills the composite's scene source (the tonemap
	// draw). Its output is currently black; this shows which textures it reads.
	if (m_graphics_bindings.pixel && state.color_count > 0) {
		const uint64_t watch = g_composite_source_addr.load(std::memory_order_relaxed);
		if (watch != 0 && state.color_info[0].desc.info.data.address == watch) {
			static std::atomic_uint watch_bind_logged {0};
			if (watch_bind_logged.fetch_add(1, std::memory_order_relaxed) < 80) {
				const auto& pimages = m_graphics_bindings.pixel->images;
				LOGF("WatchBind: frame=%d %s idx=%u ps=%d fmt=%s images=%zu\n",
				     buffer.GetContext().GetGpu().GetFrameNum(), draw.Name(), draw.index_count,
				     state.ps_active ? 1 : 0,
				     vk::to_string(state.color_info[0].desc.info.pixel_format).c_str(),
				     pimages.size());
				for (size_t slot = 0; slot < pimages.size() && slot < 6; slot++) {
					const auto& b = pimages[slot];
					LOGF("WatchBindImg[%zu]: addr=0x%010" PRIx64 " id=%u/%u ext=%ux%u"
					     " fmt=%s\n",
					     slot, b.desc.info.data.address, b.image_id.index,
					     b.image_id.generation, b.desc.info.extent.width,
					     b.desc.info.extent.height,
					     vk::to_string(b.desc.info.pixel_format).c_str());
					if (b.desc.info.extent.width == 1 && b.desc.info.extent.height == 1) {
						KytySceneDiag::exposure_addr.store(b.desc.info.data.address,
						                                   std::memory_order_relaxed);
					}
					if (b.desc.info.extent.width == 32 && b.desc.info.extent.height == 32) {
						KytySceneDiag::lut_addr.store(b.desc.info.data.address,
						                              std::memory_order_relaxed);
					}
				}
			}
		}
	}
	// AMD Windows drivers reject valid barycentric fragment pipelines with triangle
	// strips; the same shaders work as lists. Expand the draw on AMD hardware.
	bool                        strip_converted      = false;
	uint32_t                    converted_count      = 0;
	std::vector<uint32_t>       converted_indices;
	DrawIndexBufferSource       converted_source {};
	const DrawIndexBufferSource* active_index_source = &index_source;
	if (!mesh_active &&
	    (topology == vk::PrimitiveTopology::eTriangleStrip ||
	     topology == vk::PrimitiveTopology::eTriangleFan) &&
	    buffer.GetGraphics().GetPhysicalDeviceProperties().vendorID == kAmdVendorId &&
	    PixelShaderUsesBarycentrics(state)) {
		static std::atomic_uint converted_logged = 0;
		const bool provoking_last =
		    buffer.GetRegisters().GetModeControl().provoking_vtx_last;
		if (ExpandStripDrawToList(converted_indices, buffer, topology, draw, emit, index_source,
		                          primitive_restart_enable, provoking_last)) {
			converted_count         = static_cast<uint32_t>(converted_indices.size());
			converted_source.host_data = converted_indices.data();
			converted_source.size =
			    static_cast<uint64_t>(converted_indices.size()) * sizeof(uint32_t);
			converted_source.type            = vk::IndexType::eUint32;
			converted_source.guest_element_size = sizeof(uint32_t);
			active_index_source                  = &converted_source;
			topology                             = vk::PrimitiveTopology::eTriangleList;
			strip_converted                      = true;
			if (converted_logged.fetch_add(1, std::memory_order_relaxed) < 2) {
				LOGF("Render: expanded barycentric strip/fan draw to a list (%u indices)\n",
				     converted_count);
			}
		}
	}
	PreparedVertexBuffers vertex_bindings;
	PreparedIndexBuffer   index_binding;
	if (!mesh_active) {
		LogDrawPhase(draw.Name(), "PrepareVertexBuffers");
		vertex_bindings = AcquireVertexBuffers(buffer, state.vertex_info[0]);
		index_binding   = PrepareIndexBuffer(buffer, *active_index_source);
	}
	std::pair<Buffer*, uint64_t> args_buffer {};
	if (emit.indirect.address != 0) {
		const auto size = static_cast<uint64_t>(emit.indirect.count - 1) * emit.indirect.stride +
		                  (draw.IsIndexed() ? sizeof(vk::DrawIndexedIndirectCommand)
		                                    : sizeof(vk::DrawIndirectCommand));
		args_buffer = m_context.GetBufferCache().ObtainBuffer(emit.indirect.address, size, false);
		EXIT_IF((args_buffer.second & 3u) != 0);
	}
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "CreatePipeline");
	}
	auto& pipeline = m_context.GetPipelineCache().GetGraphicsPipeline(
	    std::span {state.color_info, state.color_count}, state.depth_info, vertex_stages, buffer,
	    state.ps_active ? &state.ps_input_info : nullptr, topology, primitive_restart_enable,
	    state.programs);
	if (pipeline.pipeline == nullptr) {
		// Creation failed and was logged; skip the draw so the game continues.
		g_census_pipeline_null.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	vk::ImageAspectFlags feedback_aspects;
	const auto rendering =
	    AcquireRenderTargets(buffer, state.color_info, state.color_count, state.depth_info,
	                         feedback_aspects, stages);

	// Resource preparation above may synchronously finish and restart the scheduler. From this
	// point onward, every operation targets the current command buffer and cannot touch guest
	// memory.
	auto vk_buffer = buffer.Handle();
	SetDrawDebugPhase(buffer, submit_id, draw, draw.IsIndexed() ? 0x100u : 0x200u);
	if (!mesh_active) {
		CommitVertexBuffers(vk_buffer, vertex_bindings);
	}
	if (state.ps_active && !draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x300u);
	}
	if (!CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline, stages)) {
		ResetBindings();
		return;
	}
	if (!mesh_active) {
		CommitIndexBuffer(vk_buffer, index_binding);
	}
	using ShaderRecompiler::IR::PushData;
	const auto draw_stages =
	    vk::ShaderStageFlagBits::eFragment |
	    (mesh_active ? vk::ShaderStageFlagBits::eMeshEXT : vk::ShaderStageFlagBits::eVertex);
	const auto draw_dwords =
	    PushData::GraphicsDrawDwords(mesh_active, state.ps_input_info.object_primitive_id);
	if (draw_dwords != 0) {
		std::array<uint32_t, PushData::MeshDrawDwordCount> draw_data {};
		if (mesh_active) {
			draw_data = {draw.index_count,
			             static_cast<uint32_t>(emit.vertex_offset),
			             emit.first_instance,
			             index_source.guest_element_size,
			             static_cast<uint32_t>(index_source.address),
			             static_cast<uint32_t>(index_source.address >> 32u)};
		} else {
			draw_data[0] = buffer.GetUserConfig().GetObjectId();
		}
		vk_buffer.pushConstants(pipeline.pipeline_layout, draw_stages, 0,
		                        draw_dwords * sizeof(uint32_t), draw_data.data());
	}
	if (const auto planes = vertex_stages.back().user_clip_planes; planes != 0) {
		vk_buffer.pushConstants(pipeline.pipeline_layout, draw_stages,
		                        PushData::UserClipPlaneDword * sizeof(uint32_t),
		                        std::bit_width(planes) * 4u * sizeof(uint32_t),
		                        buffer.GetRegisters().GetUserClipPlanes().data());
	}


	auto& dynamic_state = m_context.GetCommandScheduler().GetDynamicState();
	UpdateGraphicsDynamicState(buffer, dynamic_state, vertex_stages.back(), state.depth_info,
	                           rendering);
	dynamic_state.SetAttachmentFeedbackLoopAspects(feedback_aspects);
	dynamic_state.Commit(m_context.GetGraphics(), vk_buffer);

	vk::PipelineStageFlags shader_write_stages = {};
	for (const auto& stage: vertex_stages) {
		if (HasShaderBufferWrites(stage.stage)) {
			shader_write_stages |= ShaderPipelineStages(NativeShaderStage(stage.logical_stage));
		}
	}
	if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
		shader_write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	if (shader_write_stages) {
		// As for dispatches, buffer writes wait for earlier accesses to the memory.
		m_context.GetCommandScheduler().EndRendering();
		ShaderWriteHazardBarrier(vk_buffer, shader_write_stages);
	}
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x400u);
	}
	m_context.GetCommandScheduler().BeginRendering(rendering);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x500u);
	}
	if (mesh_active) {
		vk_buffer.drawMeshTasksEXT(mesh_groups, draw.instance_count, 1);
	} else if (args_buffer.first == nullptr) {
		EmitDrawPrimitives(ucfg, vk_buffer, draw, emit);
	} else if (draw.IsIndexed()) {
		vk_buffer.drawIndexedIndirect(args_buffer.first->Handle(), args_buffer.second,
		                              emit.indirect.count, emit.indirect.stride);
	} else {
		vk_buffer.drawIndirect(args_buffer.first->Handle(), args_buffer.second,
		                       emit.indirect.count, emit.indirect.stride);
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::Draws);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::DrawInstances, draw.instance_count);

	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x600u);
	}
	if (shader_write_stages) {
		m_context.GetCommandScheduler().EndRendering();
		ShaderWriteBarrier(vk_buffer, shader_write_stages);
	}
	LogDrawPhase(draw.Name(), "DrawComplete");
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x700u);
	}
	const auto completed = g_census_complete.fetch_add(1, std::memory_order_relaxed) + 1;
	if (completed % 10000 == 0) {
		m_context.GetGraphics().LogMemoryBudget();
	}
	if (completed % 1000 == 0) {
		const auto extent = state.color_count > 0 ? state.color_info[0].Extent() : vk::Extent2D {};
		LOGF("RenderCensus: idx_calls=%llu auto_calls=%llu complete=%llu (idx_empty=%llu "
		     "resolved=%llu no_vs=%llu topo_fail=%llu state_fail=%llu rectlist_skip=%llu "
		     "mesh_skip=%llu pipeline_null=%llu) last=%s count=%u colors=%u addr=0x%010llx "
		     "extent=%ux%u\n",
		     static_cast<unsigned long long>(g_census_index_calls.load(std::memory_order_relaxed)),
		     static_cast<unsigned long long>(g_census_auto_calls.load(std::memory_order_relaxed)),
		     static_cast<unsigned long long>(completed),
		     static_cast<unsigned long long>(g_census_empty.load(std::memory_order_relaxed)),
		     static_cast<unsigned long long>(g_census_resolved.load(std::memory_order_relaxed)),
		     static_cast<unsigned long long>(g_census_no_vs.load(std::memory_order_relaxed)),
		     static_cast<unsigned long long>(g_census_topology_fail.load(std::memory_order_relaxed)),
		     static_cast<unsigned long long>(g_census_state_fail.load(std::memory_order_relaxed)),
		     static_cast<unsigned long long>(g_census_rectlist_skip.load(std::memory_order_relaxed)),
		     static_cast<unsigned long long>(g_census_mesh_skip.load(std::memory_order_relaxed)),
		     static_cast<unsigned long long>(g_census_pipeline_null.load(std::memory_order_relaxed)),
		     draw.Name(), draw.index_count, state.color_count,
		     static_cast<uint64_t>(state.color_count > 0
		                               ? state.color_info[0].desc.info.data.address
		                               : 0u),
		     extent.width, extent.height);
	}
}

// Vulkan offsets for a prepared draw, or nullopt when its indirect records need CPU counts, as
// mesh and quad-list draws do. Indirect records carry their own offsets.
static std::optional<DrawEmitInfo> ResolveDrawEmit(const HW::UserConfig&        ucfg,
                                                   const ShaderVertexInputInfo& vs,
                                                   DrawOffsetSource             source,
                                                   int32_t                      first_vertex,
                                                   uint32_t                     first_instance,
                                                   const DrawIndirectArgs&      indirect) {
	if (indirect.address != 0) {
		if (vs.stage.program->stage == ShaderType::Mesh ||
		    ucfg.GetPrimType() == Prospero::PrimitiveType::kQuadListLegacy) {
			return std::nullopt;
		}
		return DrawEmitInfo {.indirect = indirect};
	}
	if (source == DrawOffsetSource::IndirectArgs) {
		// Native fetches consume the patched SGPRs; rewritten fetches need Vulkan offsets.
		return vs.fetch_embedded ? DrawEmitInfo {first_vertex, first_instance} : DrawEmitInfo {};
	}
	const auto [vertex_offset, instance_offset] = ResolveDrawOffsets(ucfg.GetIndexOffset(), vs);
	return DrawEmitInfo {vertex_offset + first_vertex, instance_offset};
}

bool RenderExecutor::DrawIndex(uint64_t submit_id, CommandBuffer& buffer,
                               const DrawIndexArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	g_census_index_calls.fetch_add(1, std::memory_order_relaxed);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndex), submit_id,
	                    args.index_count, 0, 1, args.instance_count,
	                    reinterpret_cast<uint64_t>(args.index_addr));

	Common::LockGuard lock(m_context.GetMutex());
	if (args.index_count == 0 || (args.instance_count == 0 && args.indirect.address == 0)) {
		return true;
	}

	if (ConsumeMetadataColorOperation(buffer, args.render_target_slice_offset) ||
	    DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer, args.render_target_slice_offset)) {
		g_census_resolved.fetch_add(1, std::memory_order_relaxed);
		ResetBindings();
		return true;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return true;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndex():Shader:\n");
		uc_print("GraphicsRenderDrawIndex():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndex():Parameters:\n"
		     "\t index_type_and_size = 0x%08" PRIx32 "\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t index_addr          = 0x%016" PRIx64 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t base_vertex         = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.index_type_and_size, args.index_count,
		     reinterpret_cast<uint64_t>(args.index_addr), args.instance_count,
		     static_cast<uint32_t>(args.base_vertex), args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		return true;
	}

	DrawIndexBufferSource index_source {};
	index_source.address = reinterpret_cast<uint64_t>(args.index_addr);
	switch (static_cast<Prospero::IndexType>(args.index_type_and_size)) {
		case Prospero::IndexType::kIndex16:
			index_source.type               = vk::IndexType::eUint16;
			index_source.guest_element_size = 2;
			break;
		case Prospero::IndexType::kIndex32:
			index_source.type               = vk::IndexType::eUint32;
			index_source.guest_element_size = 4;
			break;
		case Prospero::IndexType::kIndex8:
			index_source.guest_element_size = 1;
			break;
		default: EXIT("unknown index_type_and_size: %u\n", args.index_type_and_size);
	}
	index_source.size = static_cast<uint64_t>(args.index_count) * index_source.guest_element_size;
	const bool primitive_restart = ResolvePrimitiveRestart(buffer, index_source);

	std::vector<uint16_t> expanded_indices;
	if (index_source.guest_element_size == 1) {
		EXIT_NOT_IMPLEMENTED(args.index_addr == nullptr);
		const auto* src = static_cast<const uint8_t*>(args.index_addr);
		expanded_indices.resize(args.index_count);
		for (uint32_t i = 0; i < args.index_count; i++) {
			expanded_indices[i] = primitive_restart && src[i] == 0xffu ? 0xffffu : src[i];
		}
		index_source.host_data = expanded_indices.data();
		index_source.size      = expanded_indices.size() * sizeof(uint16_t);
	}

	const DrawCallInfo draw {CommandBufferDebugOp::DrawIndex, args.index_count,
	                        args.instance_count, args.first_instance};
	DrawRenderState state {};
	if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state)) {
		g_census_state_fail.fetch_add(1, std::memory_order_relaxed);
		ResetBindings();
		return true;
	}

	LogDrawStateIfNeeded(buffer, draw, state, args.index_type_and_size,
	                     args.index_addr);
	const auto emit = ResolveDrawEmit(ucfg, state.vertex_info[0], args.offset_source,
	                                  args.base_vertex, args.first_instance, args.indirect);
	if (emit) {
		ExecutePreparedDraw(submit_id, buffer, draw, state, topology, *emit, index_source,
		                    primitive_restart);
	}
	ResetBindings();
	return emit.has_value();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool RenderExecutor::DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	g_census_auto_calls.fetch_add(1, std::memory_order_relaxed);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndexAuto), submit_id,
	                    args.vertex_count, 0, args.first_vertex, args.instance_count,
	                    args.first_instance);

	Common::LockGuard lock(m_context.GetMutex());
	if (args.indirect.address == 0 && (args.vertex_count == 0 || args.instance_count == 0)) {
		return true;
	}

	if (ConsumeMetadataColorOperation(buffer, args.render_target_slice_offset) ||
	    DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer, args.render_target_slice_offset)) {
		g_census_resolved.fetch_add(1, std::memory_order_relaxed);
		ResetBindings();
		return true;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return true;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndexAuto():Shader:\n");
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndexAuto():Parameters:\n"
		     "\t vertex_count        = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t first_vertex        = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	const DrawCallInfo draw {CommandBufferDebugOp::DrawIndexAuto,
	                         args.vertex_count, args.instance_count, args.first_instance};

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		g_census_topology_fail.fetch_add(1, std::memory_order_relaxed);
		ResetBindings();
		return true;
	}
	DrawRenderState state {};
	if (!PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state)) {
		g_census_state_fail.fetch_add(1, std::memory_order_relaxed);
		ResetBindings();
		return true;
	}

	const bool rect_list = Prospero::IsRectList(ucfg.GetPrimType());
	if (rect_list && state.vertex_info[0].buffers_num == 0 &&
	    state.vertex_info[0].stage.program->param_export_mask == 0 &&
	    state.ps_input_info.input_num != 0) {
		if (graphics_debug_dump_enabled()) {
			LOGF("DrawIndexAuto: skipping rect-list draw with no VS param exports and PS inputs: "
			     "ps_inputs=%u ps=0x%016" PRIx64 " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
			     state.ps_input_info.input_num, sh_ctx.GetPs().ps_regs.data_addr,
			     sh_ctx.GetVs().es_regs.data_addr, sh_ctx.GetVs().gs_regs.data_addr);
		}
		g_census_rectlist_skip.fetch_add(1, std::memory_order_relaxed);
		ResetBindings();
		return true;
	}

	LogDrawStateIfNeeded(buffer, draw, state, 0, nullptr);
	const auto emit = ResolveDrawEmit(ucfg, state.vertex_info[0], args.offset_source,
	                                  static_cast<int32_t>(args.first_vertex), args.first_instance,
	                                  args.indirect);
	if (emit) {
		ExecutePreparedDraw(submit_id, buffer, draw, state, topology, *emit, {}, false);
	}
	ResetBindings();
	return emit.has_value();
}

bool RenderExecutor::ResolveColorTargets(CommandBuffer& buffer, uint32_t render_target_slice_offset) {
	const auto& hw = buffer.GetRegisters();
	if (hw.GetColorControl().mode != 3) {
		return false;
	}

	const auto& src_rt = hw.GetRenderTarget(0);
	const auto& dst_rt = hw.GetRenderTarget(1);
	if (src_rt.base.addr == 0 || dst_rt.base.addr == 0) {
		return false;
	}

	RenderColorInfo src {};
	RenderColorInfo dst {};
	ResolveRenderColorTarget(buffer, src, render_target_slice_offset, 0, true, true);
	ResolveRenderColorTarget(buffer, dst, render_target_slice_offset, 1, true, true);
	if (!src.image_id || !dst.image_id) {
		return false;
	}
	if (src.desc.info.data.address == dst.desc.info.data.address &&
	    src.guest_mip_level == dst.guest_mip_level &&
	    src.guest_array_layer == dst.guest_array_layer) {
		return true;
	}

	auto& cache = m_context.GetTextureCache();
	cache.MarkGpuWritten(dst.image_id);
	auto& source      = cache.GetImage(src.image_id);
	auto& destination = cache.GetImage(dst.image_id);
	destination.Resolve(source, {src.guest_mip_level, 1, src.guest_array_layer, 1},
	                    {dst.guest_mip_level, 1, dst.guest_array_layer, 1});
	return true;
}

} // namespace Libs::Graphics
