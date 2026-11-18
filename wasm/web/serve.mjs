// Minimal static server for the demo: serves <file>.br with Content-Encoding: br
// when it exists and the browser accepts brotli.
//   node serve.mjs <site dir> [port]
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
const root = path.resolve(process.argv[2] || ".");
const port = +(process.argv[3] || 8765);
const types = { ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".wasm": "application/wasm",
  ".json": "application/json", ".dat": "application/octet-stream", ".css": "text/css" };
http.createServer((req, res) => {
  const url = decodeURIComponent(new URL(req.url, "http://x").pathname);
  let file = path.join(root, url === "/" ? "index.html" : url);
  if (!file.startsWith(root)) { res.writeHead(403).end(); return; }
  const headers = { "Content-Type": types[path.extname(file)] || "application/octet-stream", "Cache-Control": "no-cache" };
  if (/\bbr\b/.test(req.headers["accept-encoding"] || "") && fs.existsSync(file + ".br")) {
    file += ".br"; headers["Content-Encoding"] = "br";
  }
  fs.stat(file, (e, st) => {
    if (e) { res.writeHead(404).end("not found"); return; }
    res.writeHead(200, { ...headers, "Content-Length": st.size });
    fs.createReadStream(file).pipe(res);
  });
}).listen(port, "127.0.0.1", () => console.log(`serving ${root} on http://127.0.0.1:${port}`));
