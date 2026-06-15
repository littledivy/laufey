use deno_core::op2;
use deno_core::v8;
use deno_core::GarbageCollected;
use deno_core::OpState;
use laufey::Window;
use serde::Serialize;
use sysinfo::{ProcessesToUpdate, System};

use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::OnceLock;

static WINDOW_ID: AtomicU32 = AtomicU32::new(0);

struct SendPtr<T>(*mut T);
unsafe impl<T> Send for SendPtr<T> {}
unsafe impl<T> Sync for SendPtr<T> {}

static ISOLATE_PTR: OnceLock<SendPtr<v8::OwnedIsolate>> = OnceLock::new();

struct SendGlobal<T>(v8::Global<T>);
unsafe impl<T> Send for SendGlobal<T> {}
unsafe impl<T> Sync for SendGlobal<T> {}

impl<T> SendGlobal<T> {
  fn get(&self) -> &v8::Global<T> {
    &self.0
  }
}

static CONTEXT: OnceLock<SendGlobal<v8::Context>> = OnceLock::new();

struct BrowserWindow;

unsafe impl GarbageCollected for BrowserWindow {
  fn trace(&self, _visitor: &mut v8::cppgc::Visitor) {}

  fn get_name(&self) -> &'static std::ffi::CStr {
    c"BrowserWindow"
  }
}

fn laufey_window() -> Window {
  Window::from_id(WINDOW_ID.load(Ordering::SeqCst))
}

#[op2]
impl BrowserWindow {
  #[constructor]
  #[cppgc]
  fn new(#[smi] width: i32, #[smi] height: i32) -> BrowserWindow {
    let window = Window::new(width, height);
    WINDOW_ID.store(window.id(), Ordering::SeqCst);
    BrowserWindow
  }

  #[fast]
  fn load(&self, #[string] url: &str) {
    laufey_window().navigate(url);
  }

  #[fast]
  fn load_html(&self, #[string] file_path: &str) {
    let path = std::path::Path::new(file_path);
    let abs = if path.is_absolute() {
      path.to_path_buf()
    } else {
      std::env::current_dir().unwrap().join(path)
    };
    laufey_window().navigate(&format!("file://{}", abs.display()));
  }

  #[fast]
  fn set_title(&self, #[string] title: &str) {
    laufey_window().set_title(title);
  }

  #[fast]
  fn set_size(&self, #[smi] width: i32, #[smi] height: i32) {
    laufey_window().set_size(width, height);
  }

  #[fast]
  fn execute_js(&self, #[string] script: &str) {
    laufey_window()
      .execute_js::<fn(Result<laufey::Value, laufey::Value>)>(script, None);
  }

  #[fast]
  fn quit(&self) {
    laufey::quit();
  }

  fn bind(
    &self,
    #[string] name: &str,
    #[scoped] callback: v8::Global<v8::Function>,
  ) {
    register_laufey_binding(name, callback);
  }
}

fn register_laufey_binding(name: &str, callback: v8::Global<v8::Function>) {
  let cb = SendGlobal(callback);
  laufey_window().add_binding(name, move |call| {
    // SAFETY: poll_js_calls() dispatches to the runtime thread,
    // so this closure runs on the same thread that owns the v8 isolate.
    let result = unsafe {
      let isolate = &mut *ISOLATE_PTR.get().unwrap().0;

      let json = {
        v8::scope!(scope, isolate);
        let ctx = v8::Local::new(scope, CONTEXT.get().unwrap().get());
        let scope = &mut v8::ContextScope::new(scope, ctx);

        let func = v8::Local::new(scope, cb.get());
        let this = v8::undefined(scope).into();
        func.call(scope, this, &[]).and_then(|result| {
          v8::json::stringify(scope, result)
            .map(|s| s.to_rust_string_lossy(scope))
        })
      };

      json
    };

    match result {
      Some(json) => call.resolve(laufey::Value::String(json)),
      None => {
        call.reject(laufey::Value::String("Handler call failed".to_string()))
      }
    }
  });
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct SystemInfo {
  hostname: String,
  os: String,
  arch: String,
  cpu_model: String,
  cores: usize,
  total_memory_mb: u64,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct LiveStats {
  cpu_usage: f32,
  per_core: Vec<f32>,
  mem_used_mb: u64,
  mem_total_mb: u64,
  uptime: u64,
}

#[op2]
#[serde]
fn op_get_system_info() -> SystemInfo {
  let mut sys = System::new_all();
  sys.refresh_all();

  let cpu_model = sys
    .cpus()
    .first()
    .map(|c| c.brand().to_string())
    .unwrap_or_default();

  SystemInfo {
    hostname: System::host_name().unwrap_or_default(),
    os: format!(
      "{} {}",
      System::name().unwrap_or_default(),
      System::os_version().unwrap_or_default()
    ),
    arch: std::env::consts::ARCH.to_string(),
    cpu_model,
    cores: sys.cpus().len(),
    total_memory_mb: sys.total_memory() / (1024 * 1024),
  }
}

#[op2]
#[serde]
fn op_get_live_stats(state: &mut OpState) -> LiveStats {
  let sys = state.borrow_mut::<System>();
  sys.refresh_cpu_usage();
  sys.refresh_memory();

  LiveStats {
    cpu_usage: sys.global_cpu_usage(),
    per_core: sys.cpus().iter().map(|c| c.cpu_usage()).collect(),
    mem_used_mb: sys.used_memory() / (1024 * 1024),
    mem_total_mb: sys.total_memory() / (1024 * 1024),
    uptime: System::uptime(),
  }
}

#[op2]
#[string]
fn op_get_processes(state: &mut OpState) -> String {
  let sys = state.borrow_mut::<System>();
  sys.refresh_processes(ProcessesToUpdate::All, true);
  let mut procs: Vec<serde_json::Value> = sys
    .processes()
    .values()
    .map(|p| {
      serde_json::json!({
          "pid": p.pid().as_u32(),
          "name": p.name().to_string_lossy(),
          "cpuUsage": (p.cpu_usage() * 100.0).round() / 100.0,
          "memoryMb": (p.memory() as f64 / (1024.0 * 1024.0) * 100.0).round() / 100.0,
      })
    })
    .collect();
  procs.sort_by(|a, b| {
    b["cpuUsage"]
      .as_f64()
      .unwrap_or(0.0)
      .partial_cmp(&a["cpuUsage"].as_f64().unwrap_or(0.0))
      .unwrap()
  });
  procs.truncate(80);
  serde_json::to_string(&procs).unwrap()
}

#[op2]
#[serde]
fn op_get_args() -> Vec<String> {
  let mut args = Vec::new();
  let mut iter = std::env::args().skip(1);
  while let Some(arg) = iter.next() {
    if arg == "--runtime" {
      iter.next(); // skip value
    } else if arg.starts_with("--runtime=") {
      // skip
    } else {
      args.push(arg);
    }
  }
  args
}

deno_core::extension!(
  ddcore,
  ops = [
    op_get_system_info,
    op_get_live_stats,
    op_get_processes,
    op_get_args
  ],
  objects = [BrowserWindow],
  esm_entry_point = "ext:ddcore/runtime.js",
  esm = ["runtime.js"],
  state = |state| {
    let mut sys = System::new();
    sys.refresh_all();
    state.put(sys);
  },
);

laufey::main!(|| {
  let rt = tokio::runtime::Runtime::new().unwrap();
  rt.block_on(async {
    let mut runtime = deno_core::JsRuntime::new(deno_core::RuntimeOptions {
      extensions: vec![ddcore::init()],
      ..Default::default()
    });

    CONTEXT.set(SendGlobal(runtime.main_context())).ok();
    ISOLATE_PTR
      .set(SendPtr(runtime.v8_isolate() as *mut _))
      .ok();

    runtime
      .run_event_loop(deno_core::PollEventLoopOptions::default())
      .await
      .ok();

    laufey::run().await;
  });
});
