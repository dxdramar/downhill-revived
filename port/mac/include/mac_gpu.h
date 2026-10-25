/* Mac build (finding 075): what port/mac/d3d11_mac.c asks of the native host's Metal device
   (port/mac/native/metal.m). Objects are host handles (never 0); descriptions are flattened to
   integers, buffers are module addresses. */
#ifndef MAC_GPU_H
#define MAC_GPU_H

#include <stdint.h>

#define GPU_IMPORT(name) __attribute__((import_module("gpu"), import_name(#name)))

enum { GPU_SRV = 1, GPU_UAV, GPU_RTV, GPU_DSV };                 /* gpu_view kinds */
enum { GPU_VS, GPU_PS, GPU_CS, GPU_STAGES };                      /* shader stages */
enum { GPU_CBV, GPU_SRV_SLOT, GPU_UAV_SLOT, GPU_SAMPLER_SLOT };    /* binding classes */
enum { GPU_LAYOUT = 1, GPU_TOPOLOGY, GPU_RASTER, GPU_BLEND, GPU_DEPTH, GPU_VERTEX_BUFFER };  /* gpu_state */

GPU_IMPORT(buffer) uint32_t gpu_buffer(uint32_t bytes, uint32_t bind, uint32_t usage, uint32_t misc, uint32_t stride,
                                       const void *init);
/* init: 0, or one D3D11_SUBRESOURCE_DATA {address, row pitch, slice pitch} per mip level */
GPU_IMPORT(texture) uint32_t gpu_texture(uint32_t width, uint32_t height, uint32_t format, uint32_t bind,
                                         uint32_t usage, uint32_t cpu, uint32_t mips, const void *init);
GPU_IMPORT(view) uint32_t gpu_view(uint32_t kind, uint32_t resource, uint32_t format, uint32_t first, uint32_t count,
                                   uint32_t raw);
GPU_IMPORT(shader) uint32_t gpu_shader(const char *entry);  /* "native:native_pixel", "exact:main" */
/* Vertex layout: count elements of {format, offset} as uint32 pairs. */
GPU_IMPORT(layout) uint32_t gpu_layout(uint32_t count, const uint32_t *elements);
/* Blend: {enable, src, dst, op, src_alpha, dst_alpha, op_alpha, write_mask} of target 0. */
GPU_IMPORT(blend) uint32_t gpu_blend(const uint32_t *desc);
GPU_IMPORT(depth) uint32_t gpu_depth(uint32_t enable, uint32_t write, uint32_t func);
GPU_IMPORT(raster) uint32_t gpu_raster(uint32_t cull, uint32_t scissor, uint32_t depth_clip);
/* Sampler: filter (D3D11_FILTER), address u, v, w, max anisotropy. */
GPU_IMPORT(sampler) uint32_t gpu_sampler(uint32_t filter, uint32_t u, uint32_t v, uint32_t w, uint32_t anisotropy);
GPU_IMPORT(query) uint32_t gpu_query(uint32_t kind);
GPU_IMPORT(free) void gpu_free(uint32_t handle);

GPU_IMPORT(set_shader) void gpu_set_shader(uint32_t stage, uint32_t shader);
GPU_IMPORT(bind) void gpu_bind(uint32_t stage, uint32_t kind, uint32_t slot, uint32_t handle);
GPU_IMPORT(state) void gpu_state(uint32_t what, uint32_t handle, uint32_t a, uint32_t b);
GPU_IMPORT(blend_factor) void gpu_blend_factor(float r, float g, float b, float a);
GPU_IMPORT(viewport) void gpu_viewport(float x, float y, float width, float height, float near, float far);
GPU_IMPORT(scissor) void gpu_scissor(int32_t left, int32_t top, int32_t right, int32_t bottom);
GPU_IMPORT(targets) void gpu_targets(uint32_t rtv, uint32_t dsv);
GPU_IMPORT(draw) void gpu_draw(uint32_t count, uint32_t start);
GPU_IMPORT(dispatch) void gpu_dispatch(uint32_t x, uint32_t y, uint32_t z);
GPU_IMPORT(clear_color) void gpu_clear_color(uint32_t rtv, float r, float g, float b, float a);
GPU_IMPORT(clear_depth) void gpu_clear_depth(uint32_t dsv, float depth);
/* box: {left, top, front, right, bottom, back} or 0 for the whole subresource */
GPU_IMPORT(copy) void gpu_copy(uint32_t dst, uint32_t x, uint32_t y, uint32_t src, const uint32_t *box);
GPU_IMPORT(update) void gpu_update(uint32_t dst, const uint32_t *box, const void *data, uint32_t pitch);
/* Waits for the work so far, then copies the resource out (rows of pitch bytes). */
GPU_IMPORT(read) void gpu_read(uint32_t src, void *out, uint32_t pitch);
GPU_IMPORT(query_end) void gpu_query_end(uint32_t query);
/* 1 when the query's work is done (its value in out, size bytes); flush: submit the work first. */
GPU_IMPORT(query_get) uint32_t gpu_query_get(uint32_t query, void *out, uint32_t size, uint32_t flush);
GPU_IMPORT(flush) void gpu_flush(void);

/* The game's window: opened by present.c, its picture shown by gpu_present. */
GPU_IMPORT(window_open) uint32_t gpu_window_open(uint32_t width, uint32_t height, uint32_t mode);
GPU_IMPORT(window_mode) void gpu_window_mode(uint32_t width, uint32_t height, uint32_t mode);
GPU_IMPORT(window_title) void gpu_window_title(const char *utf8);
GPU_IMPORT(window_closed) uint32_t gpu_window_closed(void);
GPU_IMPORT(window_close) void gpu_window_close(void);
/* The back buffer (a texture handle) at that size, made anew when the size changes. */
GPU_IMPORT(back_buffer) uint32_t gpu_back_buffer(uint32_t width, uint32_t height);
GPU_IMPORT(present) void gpu_present(void);
GPU_IMPORT(adapter_name) uint32_t gpu_adapter_name(char *out, uint32_t size);

#endif
