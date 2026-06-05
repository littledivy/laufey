const {
  BrowserWindow,
  op_get_system_info,
  op_get_live_stats,
  op_get_processes,
} = Deno.core.ops;

const win = new BrowserWindow(1024, 768);
win.setTitle("LAUFEY System Dashboard");
win.bind("getSystemInfo", () => op_get_system_info());
win.bind("getStats", () => op_get_live_stats());
win.bind("getProcesses", () => JSON.parse(op_get_processes()));
win.loadHtml("index.html");
