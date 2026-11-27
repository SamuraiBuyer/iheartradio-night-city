// Build our inkatlas JSON from the vanilla radio-station atlas JSON: same settings, our texture, one full-size part.
// usage: node make_atlas.js <vanilla.inkatlas.json> <out.inkatlas.json> <xbm depot path> <part name>
const fs = require("fs");
const [src, out, xbm, part] = process.argv.slice(2);
const j = JSON.parse(fs.readFileSync(src, "utf8"));
const rc = j.Data.RootChunk;
const mapper = {
  "$type": "inkTextureAtlasMapper",
  clippingRectInPixels: { "$type": "Rect", bottom: 0, left: 0, right: 0, top: 0 },
  clippingRectInUVCoords: { "$type": "RectF", Bottom: 1, Left: 0, Right: 1, Top: 0 },
  partName: { "$type": "CName", "$storage": "string", "$value": part },
};
for (const slot of rc.slots.Elements) {
  slot.texture.DepotPath = { "$type": "ResourcePath", "$storage": "string", "$value": xbm };
  slot.parts = [JSON.parse(JSON.stringify(mapper))];
}
delete j.Header.ArchiveFileName;
fs.writeFileSync(out, JSON.stringify(j, null, 2));
console.log("slots:", rc.slots.Elements.length, "->", xbm, "part", part);
