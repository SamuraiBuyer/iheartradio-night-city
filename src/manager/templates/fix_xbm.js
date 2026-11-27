// Give an imported icon texture the exact texture setup of the vanilla radio-station icons
// (UI group, gamma, not streamed, downgrade allowed). usage: node fix_xbm.js <in.xbm.json> <vanilla.xbm.json>
const fs = require("fs");
const [file, vanillaFile] = process.argv.slice(2);
const j = JSON.parse(fs.readFileSync(file, "utf8"));
const v = JSON.parse(fs.readFileSync(vanillaFile, "utf8"));
const setup = j.Data.RootChunk.setup, vs = v.Data.RootChunk.setup;
for (const k of ["group", "compression", "rawFormat", "isGamma", "isStreamable", "allowTextureDowngrade", "hasMipchain"]) {
  setup[k] = vs[k];
}
delete j.Header.ArchiveFileName;
fs.writeFileSync(file, JSON.stringify(j, null, 2));
console.log("size", j.Data.RootChunk.width, "x", j.Data.RootChunk.height, "setup", JSON.stringify(setup));
