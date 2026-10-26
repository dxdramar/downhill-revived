//! The device: one per process, never released (task A2).
//!
//! Settings: wgpu-native v29.0.1.1's as the retired C renderer's `port/gs/gpu_wgpu.c` drove it
//! (last at render/rust 3882d5576; the line numbers below are that file's), written out explicitly
//! (finding 085 question 1):
//! - InstanceFlags empty, no `with_env()`; DEBUG | VALIDATION only for `DHDR_DEV_DEBUG` /
//!   `DHDR_DEV_VALIDATION`. Backends DX12, or VULKAN for `DHDR_DEV_VULKAN`.
//! - DX12 compiler: FXC for `DHDR_DEV_FXC` or no `dxc_path`, else `DynamicDxc { dxc_path }`; every
//!   other DX12 option default. No memory budget thresholds.
//! - Adapter: `DHDR_DEV_FALLBACK` = the fallback (WARP) adapter; else the enumerated adapter with
//!   the PCI ids (not a CPU one), else high performance.
//! - Device: the adapter's own limits; DUAL_SOURCE_BLENDING when offered and not refused
//!   (`DHDR_DEV_NO_DUAL_SOURCE`); default memory hints; trace off.
//! - `guard::install_error_handler` right after creation; `log::set_level(log_level)`.
//! - `DHDR_FEAT_RENDER_PIPELINES`: present.wgsl's pipeline (an RGBA8 target) built inside a
//!   Validation scope, as the probe.
//!
//! The retired C path's device_init: `port/gs/gpu_wgpu.c:122-218`. The C side now is
//! `port/gs/gpu_rust.c`'s device_init, which only turns the DHD_WGPU_* variables into flags.
use crate::abi::*;
use std::ffi::{CStr, CString};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};

/// What C asked for (`DhdrDeviceDesc`, checked and copied by `ffi.rs`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DeviceOptions {
    /// `DHDR_DEV_*`
    pub flags: u32,
    /// PCI ids of the presentation adapter; 0, 0: high performance.
    pub vendor_id: u32,
    pub device_id: u32,
    /// `DHDR_LOG_*`
    pub log_level: u32,
    /// dxcompiler.dll, or None (FXC).
    pub dxc_path: Option<String>,
}

/// The device and what was learnt making it. Send + Sync (natively; on wasm through wgpu's
/// `fragile-send-sync-non-atomic-wasm`), so it lives in a global.
pub struct Gpu {
    pub instance: wgpu::Instance,
    pub adapter: wgpu::Adapter,
    pub device: wgpu::Device,
    pub queue: wgpu::Queue,
    pub info: wgpu::AdapterInfo,
    /// The adapter's name, NUL-terminated (`dhdr_adapter_name`).
    pub name: CString,
    /// `DHDR_FEAT_*`
    pub features: u32,
    pub options: DeviceOptions,
    /// The native renderer's shared objects, made on first use from any thread (`native/shared.rs`).
    pub shared: OnceLock<crate::native::shared::NativeShared>,
}

impl Gpu {
    /// Makes a device as `o` asks (natively; blocks). Not for wasm: there `web/init.rs` makes the
    /// device asynchronously and `install`s it.
    pub fn create(o: &DeviceOptions) -> Result<Gpu, String> {
        #[cfg(all(feature = "native", not(target_arch = "wasm32")))]
        {
            native::create(o)
        }
        #[cfg(not(all(feature = "native", not(target_arch = "wasm32"))))]
        {
            let _ = o;
            Err("device::Gpu::create: native only (the browser's device comes from web/init.rs)".into())
        }
    }

    /// The `Gpu` of a device already made (natively by `create`; in the browser by the async
    /// init): the name and the features (dual-source as the device has it, RENDER_PIPELINES as
    /// `render_pipelines` says).
    pub fn from_parts(instance: wgpu::Instance, adapter: wgpu::Adapter, device: wgpu::Device, queue: wgpu::Queue,
                      options: DeviceOptions, render_pipelines: bool) -> Gpu {
        let info = adapter.get_info();
        let name = CString::new(info.name.replace('\0', "")).unwrap_or_default();
        let mut features = 0;
        if device.features().contains(wgpu::Features::DUAL_SOURCE_BLENDING) {
            features |= DHDR_FEAT_DUAL_SOURCE;
        }
        if render_pipelines {
            features |= DHDR_FEAT_RENDER_PIPELINES;
        }
        Gpu { instance, adapter, device, queue, info, name, features, options, shared: OnceLock::new() }
    }
}

/// The DX12 compiler `o` asks for: FXC for `DHDR_DEV_FXC` or no dxcompiler.dll, else DXC from it.
pub fn shader_compiler(o: &DeviceOptions) -> wgpu::Dx12Compiler {
    match &o.dxc_path {
        Some(path) if o.flags & DHDR_DEV_FXC == 0 => wgpu::Dx12Compiler::DynamicDxc { dxc_path: path.clone() },
        _ => wgpu::Dx12Compiler::Fxc,
    }
}

/// The instance flags `o` asks for: none, or DEBUG / VALIDATION for `DHDR_DEV_DEBUG` /
/// `DHDR_DEV_VALIDATION` (wgpu-native's `WGPUInstanceFlag_*`; never `with_env()`).
pub fn instance_flags(o: &DeviceOptions) -> wgpu::InstanceFlags {
    let mut f = wgpu::InstanceFlags::empty();
    if o.flags & DHDR_DEV_DEBUG != 0 {
        f |= wgpu::InstanceFlags::DEBUG;
    }
    if o.flags & DHDR_DEV_VALIDATION != 0 {
        f |= wgpu::InstanceFlags::VALIDATION;
    }
    f
}

/// The window surface's format, and the present pipelines' target: RGBA8, except on Apple
/// platforms, whose CAMetalLayer offers no RGBA8 (BGRA8 holds the same values in another order).
#[cfg(target_vendor = "apple")]
pub const SURFACE_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Bgra8Unorm;
#[cfg(not(target_vendor = "apple"))]
pub const SURFACE_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8Unorm;

/// The backend `o` asks for: DX12, or Vulkan for `DHDR_DEV_VULKAN`; Metal on Apple platforms.
#[cfg(target_vendor = "apple")]
pub fn backends(_o: &DeviceOptions) -> wgpu::Backends {
    wgpu::Backends::METAL
}

/// The backend `o` asks for: DX12, or Vulkan for `DHDR_DEV_VULKAN`; Metal on Apple platforms.
#[cfg(not(target_vendor = "apple"))]
pub fn backends(o: &DeviceOptions) -> wgpu::Backends {
    if o.flags & DHDR_DEV_VULKAN != 0 {
        wgpu::Backends::VULKAN
    } else {
        wgpu::Backends::DX12
    }
}

/// The probe of `DHDR_FEAT_RENDER_PIPELINES`: present.wgsl's pipeline with an RGBA8 target, built
/// inside a Validation scope. Returns the scope; the caller pops it (None = the probe was built).
pub fn render_probe(device: &wgpu::Device) -> wgpu::ErrorScopeGuard {
    let scope = device.push_error_scope(wgpu::ErrorFilter::Validation);
    let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("present.wgsl (probe)"),
        source: wgpu::ShaderSource::Wgsl(crate::shaders::PRESENT.into()),
    });
    let _pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("render probe"),
        layout: None,
        vertex: wgpu::VertexState {
            module: &module,
            entry_point: Some("present_vertex"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            buffers: &[],
        },
        primitive: wgpu::PrimitiveState::default(),
        depth_stencil: None,
        multisample: wgpu::MultisampleState::default(),
        fragment: Some(wgpu::FragmentState {
            module: &module,
            entry_point: Some("present_fragment"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            targets: &[Some(wgpu::ColorTargetState {
                format: wgpu::TextureFormat::Rgba8Unorm,
                blend: None,
                write_mask: wgpu::ColorWrites::ALL,
            })],
        }),
        multiview_mask: None,
        cache: None,
    });
    scope
}

#[cfg(all(feature = "native", not(target_arch = "wasm32")))]
mod native {
    use super::*;

    fn instance(o: &DeviceOptions, compiler: wgpu::Dx12Compiler) -> wgpu::Instance {
        let mut desc = wgpu::InstanceDescriptor::new_without_display_handle();
        desc.backends = backends(o);
        desc.flags = instance_flags(o);
        desc.memory_budget_thresholds = wgpu::MemoryBudgetThresholds { for_resource_creation: None, for_device_loss: None };
        desc.backend_options = wgpu::BackendOptions {
            gl: wgpu::GlBackendOptions::default(),
            dx12: wgpu::Dx12BackendOptions { shader_compiler: compiler, ..Default::default() },
            noop: wgpu::NoopBackendOptions::default(),
        };
        wgpu::Instance::new(desc)
    }

    /// gpu_wgpu.c:122-218 with wgpu-native's settings (the module comment), except that WARP always
    /// compiles with FXC (`software_fxc`).
    pub fn create(o: &DeviceOptions) -> Result<Gpu, String> {
        crate::log::set_level(o.log_level);
        let backends = backends(o);
        let compiler = shader_compiler(o);
        let mut instance = instance(o, compiler.clone());
        let mut adapter = adapter(&instance, backends, o)?;
        if software_fxc(&compiler, &adapter.get_info()) {
            crate::host::log("dhd_render: shaders for the WARP (software) adapter compiled with FXC, not DXC");
            drop(adapter);
            instance = self::instance(o, wgpu::Dx12Compiler::Fxc);
            adapter = self::adapter(&instance, backends, o)?;
        }
        let info = adapter.get_info();
        let dual = adapter.features().contains(wgpu::Features::DUAL_SOURCE_BLENDING)
            && o.flags & DHDR_DEV_NO_DUAL_SOURCE == 0;
        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: None,
            required_features: if dual { wgpu::Features::DUAL_SOURCE_BLENDING } else { wgpu::Features::empty() },
            // the adapter's own, so a 4 MB bin list always fits (gpu_wgpu.c:195)
            required_limits: adapter.limits(),
            experimental_features: wgpu::ExperimentalFeatures::disabled(),
            memory_hints: wgpu::MemoryHints::default(),
            trace: wgpu::Trace::Off,
        }))
        .map_err(|e| format!("request_device on {} ({:?}): {e}", info.name, info.backend))?;
        crate::guard::install_error_handler(&device);
        let render_pipelines = pollster::block_on(render_probe(&device).pop()).is_none();
        Ok(Gpu::from_parts(instance, adapter, device, queue, o.clone(), render_pipelines))
    }

    /// Whether DXC was asked for and the adapter is WARP (a software DX12 adapter: CPU type, or
    /// Microsoft's 0x1414:0x8C). DXIL from DXC takes WARP tens of minutes to compile where FXC's
    /// DXBC takes seconds, on wgpu-native as here (finding 085, gate J1 open item; finding 086,
    /// F1), so the device is made again with FXC. Exact pictures are the same with either
    /// compiler (gate J1). The options kept (`Gpu::options`) stay what C asked for.
    fn software_fxc(compiler: &wgpu::Dx12Compiler, info: &wgpu::AdapterInfo) -> bool {
        matches!(compiler, wgpu::Dx12Compiler::DynamicDxc { .. })
            && info.backend == wgpu::Backend::Dx12
            && (info.device_type == wgpu::DeviceType::Cpu || (info.vendor == 0x1414 && info.device == 0x8C))
    }

    /// gpu_wgpu.c:163-189: the fallback (WARP) adapter for `DHDR_DEV_FALLBACK`; else the
    /// enumerated adapter with the presentation adapter's PCI ids (never a CPU one); else the
    /// high-performance one.
    fn adapter(instance: &wgpu::Instance, backends: wgpu::Backends, o: &DeviceOptions) -> Result<wgpu::Adapter, String> {
        let fallback = o.flags & DHDR_DEV_FALLBACK != 0;
        if !fallback && o.vendor_id != 0 {
            let found = pollster::block_on(instance.enumerate_adapters(backends)).into_iter().find(|a| {
                let i = a.get_info();
                i.vendor == o.vendor_id && i.device == o.device_id && i.device_type != wgpu::DeviceType::Cpu
            });
            if let Some(a) = found {
                return Ok(a);
            }
        }
        pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            force_fallback_adapter: fallback,
            compatible_surface: None,
        }))
        .map_err(|e| format!("no {}adapter ({backends:?}): {e}", if fallback { "fallback " } else { "" }))
    }
}

static GPU: OnceLock<Gpu> = OnceLock::new();
static FAILED: AtomicBool = AtomicBool::new(false);
static INIT: Mutex<()> = Mutex::new(());

/// The process's device (`dhdr_device_init`): true when it exists (later calls return true at once,
/// whatever `o` holds); false when it failed, and every later call returns false too.
pub fn init(o: &DeviceOptions) -> bool {
    if GPU.get().is_some() {
        return true;
    }
    let _lock = INIT.lock().unwrap_or_else(|p| p.into_inner());
    if GPU.get().is_some() {
        return true;
    }
    if FAILED.load(Ordering::Acquire) {
        return false;
    }
    crate::log::set_level(o.log_level);
    match Gpu::create(o) {
        Ok(g) => install(g),
        Err(e) => {
            crate::host::log(&format!("dhd_render: no device: {e}"));
            FAILED.store(true, Ordering::Release);
            false
        }
    }
}

/// Makes `g` the process's device (the browser's async init). False if there is one already.
pub fn install(g: Gpu) -> bool {
    GPU.set(g).is_ok()
}

/// Latches a failed init (the browser's async init failed).
pub fn fail() {
    FAILED.store(true, Ordering::Release);
}

/// The process's device, if made.
pub fn gpu() -> Option<&'static Gpu> {
    GPU.get()
}

/// The adapter's name ("" before init).
pub fn adapter_name() -> &'static CStr {
    GPU.get().map_or(c"", |g| g.name.as_c_str())
}

/// `DHDR_FEAT_*` (0 before init).
pub fn features() -> u32 {
    GPU.get().map_or(0, |g| g.features)
}

/// `DHDR_FEAT_DUAL_SOURCE` of `g`.
pub fn has_dual_source(g: &Gpu) -> bool {
    g.features & DHDR_FEAT_DUAL_SOURCE != 0
}
