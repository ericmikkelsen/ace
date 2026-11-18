// Runs the parser in a Web Worker so the page never blocks.
//
// Messages in:   {type: "init", args}           load grammar.json + grammar.dat, start the engine
//                {type: "parse", id, text}      parse each non-empty line
// Messages out:  {type: "progress", loaded, total}
//                {type: "ready", bytes, ms, memoryMB}
//                {type: "result", id, sentence, readings: [{mrs, pretty, derivation}], capped, ms, memoryMB}
//                {type: "done", id}
//                {type: "restart"}              engine needs a fresh image; recreate the worker
//                {type: "error", message}
importScripts("ace-web.js");

let ace = null;
let out = [], err = [];

const memoryMB = () => Math.round(ace.HEAPU8.length / 1e6);

// MRS on one line is hard to read; put each relation on its own line.
// Done here so the page only has to insert text.
function prettyMrs(m) {
  return m.replace(/ RELS: < /, "\n RELS: <\n   ")
          .replace(/ \]  \[ /g, " ]\n   [ ")
          .replace(/ > HCONS: < /, " >\n HCONS: < ")
          .replace(/ > ICONS: < /, " >\n ICONS: < ");
}

async function init(args) {
  const t0 = performance.now();
  ace = await createAce({ print: (l) => out.push(l), printErr: (l) => err.push(l) });
  const meta = await (await fetch("grammar.json")).json();
  const base = ace._ace_web_reserve(meta.bytes);
  // the server sends grammar.dat brotli-compressed with Content-Encoding: br,
  // so the browser decompresses it and we copy each chunk straight into place
  const res = await fetch(meta.file);
  if (!res.ok) throw new Error(`fetching ${meta.file}: HTTP ${res.status}`);
  const reader = res.body.getReader();
  let off = 0, lastReport = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    if (off + value.length > meta.bytes) throw new Error("grammar image is larger than grammar.json says");
    ace.HEAPU8.set(value, base + off); // HEAPU8 is re-read each time: memory may have grown
    off += value.length;
    if (off - lastReport >= meta.bytes / 100 || off === meta.bytes) {   // about 100 updates, not one per chunk
      lastReport = off;
      postMessage({ type: "progress", loaded: off, total: meta.bytes });
    }
  }
  if (off !== meta.bytes) throw new Error(`grammar image is ${off} bytes, expected ${meta.bytes}`);
  out = []; err = [];
  ace.ccall("ace_web_init", "number", ["number", "string"], [meta.bytes, args]);
  postMessage({ type: "ready", bytes: meta.bytes, ms: Math.round(performance.now() - t0), memoryMB: memoryMB() });
}

function parseLine(id, line) {
  out = []; err = [];
  const t0 = performance.now();
  ace.ccall("ace_web_parse", "number", ["string"], [line]);
  const ms = performance.now() - t0;
  const readings = out
    .filter((l) => l.startsWith("["))
    .map((l) => {
      const i = l.indexOf(" ;  ");
      const mrs = i < 0 ? l : l.slice(0, i);
      return { mrs, pretty: prettyMrs(mrs), derivation: i < 0 ? "" : l.slice(i + 4) };
    });
  const capped = err.some((l) => l.includes("too much RAM") || l.includes("hit RAM limit"));
  postMessage({ type: "result", id, sentence: line, readings, capped, ms: Math.round(ms), memoryMB: memoryMB() });
}

onmessage = async (e) => {
  const m = e.data;
  try {
    if (m.type === "init") await init(m.args || "");
    else if (m.type === "parse") {
      for (const line of m.text.split("\n").map((s) => s.trim()).filter(Boolean)) parseLine(m.id, line);
      postMessage({ type: "done", id: m.id });
    }
  } catch (x) {
    // exit(77): the engine's internal counter wrapped and it needs a pristine image
    if (x && x.name === "ExitStatus" && x.status === 77) postMessage({ type: "restart" });
    else postMessage({ type: "error", message: String((x && x.message) || x) });
  }
};
