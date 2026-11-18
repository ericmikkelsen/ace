// Exercise the browser module's API under Node: stream an image into the
// reserved region, initialise, and parse lines from a file.
//   node node-test.cjs <ace-web.js> <grammar.dat> <sentences.txt> [engine args...]
const fs = require("fs");
const [modPath, imagePath, inputPath, ...args] = process.argv.slice(2);
const createAce = require(require("path").resolve(modPath));
(async () => {
  const out = [], err = [];
  const t0 = Date.now();
  const ace = await createAce({ print: l => out.push(l), printErr: l => err.push(l) });
  const bytes = fs.statSync(imagePath).size;
  const base = ace._ace_web_reserve(bytes);
  const fd = fs.openSync(imagePath, "r");
  const chunk = Buffer.alloc(4 << 20);
  for (let off = 0; off < bytes; ) {
    const n = fs.readSync(fd, chunk, 0, chunk.length, off);
    ace.HEAPU8.set(chunk.subarray(0, n), base + off);   // re-read HEAPU8: memory may have grown
    off += n;
  }
  ace.ccall("ace_web_init", "number", ["number", "string"], [bytes, args.join(" ")]);
  const t1 = Date.now();
  for (const line of fs.readFileSync(inputPath, "utf8").split("\n")) {
    if (!line) continue;
    ace.ccall("ace_web_parse", "number", ["string"], [line]);
  }
  const t2 = Date.now();
  process.stdout.write(out.join("\n") + "\n");
  process.stderr.write(err.join("\n") + `\nload ${t1 - t0} ms, parse ${t2 - t1} ms, wasm memory ${(ace.HEAPU8.length / 1e6).toFixed(0)} MB\n`);
})();
