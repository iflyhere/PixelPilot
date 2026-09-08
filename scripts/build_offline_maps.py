#!/usr/bin/env python3
"""Builds the offline MBTiles the headset HUD reads: a basemap and a terrain height model.

The immersive HUD's minimap and height profile work with no connection, which is the situation
the app is flown in - so the tiles have to be on the device beforehand. This produces both
files from sources that may actually be redistributed:

  basemap   basemap.de Web Raster, published by the BKG for the AdV under Datenlizenz
            Deutschland - Namensnennung 2.0. Germany only, but genuinely open, which the
            usual OpenStreetMap tile servers are not: their tile usage policy rules out bulk
            downloading, so they are not an option here however convenient they look.

  imagery   Baden-Wuerttemberg orthophotos at 20 cm a pixel, from the LGL's WMS, also
            dl-de/by-2-0 and marked "Unentgeltliche Nutzung nach Open Data Lizenz". For flying
            this beats a street map: you recognise the actual treeline and the actual field
            edge you are looking at, which is not something a road casing tells you. Requires
            Pillow, because a WMS serves images by bounding box rather than tiles - see
            fetch_block().

  terrain   The Terrain Tiles open dataset on AWS, terrarium encoding. Global, no account.

Both licences require naming the source. The attribution string goes into the MBTiles metadata
and the HUD renders it on the minimap, so a built file carries its own credit.

Usage, for the whole of Baden-Wuerttemberg plus detail around a flying site:

    python scripts/build_offline_maps.py basemap --bbox 7.4,47.4,10.6,49.9 --zooms 11-12
    python scripts/build_offline_maps.py terrain --bbox 7.4,47.4,10.6,49.9 --zooms 11
    python scripts/build_offline_maps.py imagery --center 48.52,9.06 --radius 3 --zooms 16-18

The last one is the one that makes a minimap sharp. The minimap draws max(240 m, 2.6x the
distance from home) across 512 pixels, so close in it wants about half a metre per pixel - which
is zoom 17 or 18 - and it only wants that within a few kilometres of where you took off. A tight
high-zoom box plus a wide coarse one is far cheaper than either alone:

    imagery --center <site> --radius 3  --zooms 16-18     ~4500 tiles, ~90 MB, sharp
    imagery --center <site> --radius 25 --zooms 13-15     ~5000 tiles, ~100 MB, context

Metres per pixel by zoom, at this latitude - each level is four times the tiles:

    z11  51      z13  13      z15  3.2     z17  0.8
    z12  25      z14  6.3     z16  1.6     z18  0.4

The minimap picks the closest zoom its file has for the span it is drawing, so a single file
holding a wide coarse layer and a narrow sharp one covers every range. Build it in two passes
with --append:

    imagery --center 48.52,9.06 --radius 25 --zooms 13-15 -o imagery.mbtiles
    imagery --center 48.52,9.06 --radius 3  --zooms 16-18 -o imagery.mbtiles --append

The height profile is not fussy about zoom - measured, z11 and z12 give the same error, because
the source resolution is the limit and not the tile grid. One low zoom over a whole state is the
right answer there.

Copy the results to the headset with:

    adb push imagery.mbtiles terrain-bw.mbtiles /sdcard/Download/

and pick them up in the app under the offline map settings.
"""

import argparse
import math
import os
import queue
import sqlite3
import sys
import threading
import time
import urllib.error
import urllib.request

SOURCES = {
    "basemap": {
        "url": "https://sgx.geodatenzentrum.de/wmts_basemapde/tile/1.0.0/"
               "de_basemapde_web_raster_{style}/default/GLOBAL_WEBMERCATOR/{z}/{y}/{x}.png",
        "format": "png",
        "encoding": None,
        # Grey by default: the minimap sits under a HUD, and a full colour map fights it.
        "style": "grau",
        "attribution": "basemap.de / BKG (dl-de/by-2-0)",
    },
    "imagery": {
        # A WMS, not a tile server: images come back for a bounding box, so whole blocks of
        # tiles are fetched at once and cut up locally. That is also what keeps this polite -
        # a block of 8x8 is one request where tiles would be sixty-four.
        "wms": "https://owsproxy.lgl-bw.de/owsproxy/ows/WMS_LGL-BW_ATKIS_DOP_20_C",
        "wms_layer": "IMAGES_DOP_20_RGB",
        "url": None,
        # JPEG, because these are photographs: the same tile is about a tenth the size of a
        # PNG and no worse to look at on a 512 px minimap.
        "format": "jpg",
        "encoding": None,
        "style": None,
        "attribution": "Orthophotos: LGL Baden-Wuerttemberg (dl-de/by-2-0)",
    },
    "terrain": {
        "url": "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png",
        "format": "png",
        # Read by TerrainDem to pick a decoder. Terrarium is (R*256 + G + B/256) - 32768
        # metres, which is NOT Mapbox's terrain-rgb; the wrong decoder gives plausible
        # nonsense rather than an error, so this key is not optional.
        "encoding": "terrarium",
        "style": None,
        "attribution": "Terrain Tiles on AWS Open Data (SRTM, GMTED2010, 3DEP, ETOPO1)",
    },
}

USER_AGENT = "PixelPilot-offline-map-builder/1.0"

# Web Mercator half-circumference, for turning tile indices into a WMS bounding box.
MERCATOR_R = 20037508.342789244

# Tiles per side in one WMS request. Eight is 2048x2048 pixels, well inside the service's
# 15000 px limit, and it turns a statewide job from tens of thousands of requests into
# hundreds.
BLOCK = 8

# A few at a time with a short pause. These are public services doing us a favour; the job is
# minutes either way, and there is no reason to make it look like an attack. The WMS gets fewer
# threads still - it renders each request rather than serving a file off a disk.
THREADS = 4
WMS_THREADS = 2
PAUSE_S = 0.02


def tile_x(lon, z):
    return (lon + 180.0) / 360.0 * (1 << z)


def tile_y(lat, z):
    r = math.radians(lat)
    return (1.0 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2.0 * (1 << z)


def parse_zooms(text):
    if "-" in text:
        lo, hi = text.split("-", 1)
        return list(range(int(lo), int(hi) + 1))
    return [int(text)]


def bbox_from_args(args):
    if args.bbox:
        w, s, e, n = (float(v) for v in args.bbox.split(","))
        return w, s, e, n
    lat, lon = (float(v) for v in args.center.split(","))
    # A radius in kilometres as a degree box. Longitude degrees shrink with latitude.
    dlat = args.radius / 111.32
    dlon = args.radius / (111.32 * math.cos(math.radians(lat)))
    return lon - dlon, lat - dlat, lon + dlon, lat + dlat


def tile_list(bbox, zooms):
    w, s, e, n = bbox
    out = []
    for z in zooms:
        x0, x1 = int(tile_x(w, z)), int(tile_x(e, z))
        y0, y1 = int(tile_y(n, z)), int(tile_y(s, z))
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                out.append((z, x, y))
    return out


def open_db(path, source, bbox, zooms, name, append):
    """
    Creates the MBTiles, or reopens one to add zoom levels to it.

    <p>Appending exists because a useful map is two passes: a wide coarse layer for getting
    your bearings and a narrow sharp one for where you actually fly. The minimap picks whatever
    zoom is closest to the span it needs, so both belong in one file.
    """
    if append and os.path.exists(path):
        db = sqlite3.connect(path)
        # Widen the recorded zoom range rather than replacing it, or the reader will refuse
        # the levels that were already there.
        have = dict(db.execute("SELECT name, value FROM metadata").fetchall())
        lo = min(int(have.get("minzoom", 99)), min(zooms))
        hi = max(int(have.get("maxzoom", -1)), max(zooms))
        db.execute("UPDATE metadata SET value=? WHERE name='minzoom'", (str(lo),))
        db.execute("UPDATE metadata SET value=? WHERE name='maxzoom'", (str(hi),))
        # The bounds too: a coarse pass is usually much wider than the sharp one it is being
        # appended to, and leaving the narrow box recorded would misdescribe the file.
        try:
            old_box = [float(v) for v in have["bounds"].split(",")]
            box = (min(old_box[0], bbox[0]), min(old_box[1], bbox[1]),
                   max(old_box[2], bbox[2]), max(old_box[3], bbox[3]))
            db.execute("UPDATE metadata SET value=? WHERE name='bounds'",
                       ("%s,%s,%s,%s" % box,))
        except (KeyError, ValueError, IndexError):
            pass  # a file without usable bounds still works; the app does not read them
        db.commit()
        print("appending to %s, which already has zooms %s..%s"
              % (path, have.get("minzoom"), have.get("maxzoom")))
        return db
    if os.path.exists(path):
        os.remove(path)
    db = sqlite3.connect(path)
    db.execute("CREATE TABLE metadata (name TEXT, value TEXT)")
    db.execute("CREATE TABLE tiles (zoom_level INTEGER, tile_column INTEGER,"
               " tile_row INTEGER, tile_data BLOB)")
    db.execute("CREATE UNIQUE INDEX tile_index ON tiles (zoom_level, tile_column, tile_row)")
    rows = [
        ("name", name),
        ("format", source["format"]),
        ("type", "baselayer"),
        ("version", "1"),
        ("minzoom", str(min(zooms))),
        ("maxzoom", str(max(zooms))),
        ("bounds", "%s,%s,%s,%s" % (bbox[0], bbox[1], bbox[2], bbox[3])),
        ("attribution", source["attribution"]),
    ]
    if source["encoding"]:
        rows.append(("encoding", source["encoding"]))
    db.executemany("INSERT INTO metadata VALUES (?, ?)", rows)
    db.commit()
    return db


def tile_bounds_3857(z, x, y, span=1):
    """The Web Mercator bounding box of a span x span block of tiles at (x, y)."""
    size = 2.0 * MERCATOR_R / (1 << z)
    west = -MERCATOR_R + x * size
    north = MERCATOR_R - y * size
    return west, north - size * span, west + size * span, north


def fetch_block(source, z, x, y, span):
    """
    One WMS request covering span x span tiles, cut into individual tiles.

    <p>Returns a dict of (x, y) -> encoded bytes. Tiles the service has no imagery for come
    back pure white; those are dropped rather than stored, which is what keeps a box that
    overlaps the state border from being mostly blank filler.
    """
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("the imagery source needs Pillow: python -m pip install pillow")
    import io

    minx, miny, maxx, maxy = tile_bounds_3857(z, x, y, span)
    px = 256 * span
    url = (
        "%s?SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=%s&STYLES="
        "&SRS=EPSG:3857&BBOX=%.4f,%.4f,%.4f,%.4f&WIDTH=%d&HEIGHT=%d&FORMAT=image/jpeg"
        % (source["wms"], source["wms_layer"], minx, miny, maxx, maxy, px, px)
    )
    blob = fetch(url)
    if blob is None:
        return {}
    block = Image.open(io.BytesIO(blob)).convert("RGB")
    out = {}
    for dy in range(span):
        for dx in range(span):
            tile = block.crop((dx * 256, dy * 256, dx * 256 + 256, dy * 256 + 256))
            lo, hi = tile.convert("L").getextrema()
            if lo == 255 and hi == 255:
                continue  # nodata, outside the state
            buf = io.BytesIO()
            tile.save(buf, "JPEG", quality=82, optimize=True)
            out[(x + dx, y + dy)] = buf.getvalue()
    return out


def fetch(url):
    """The tile bytes, None for a tile the source does not have, or raises after retries."""
    last = None
    for attempt in range(4):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code in (404, 204):
                return None  # outside coverage, not a failure
            last = e
        except Exception as e:
            last = e
        time.sleep(1 + attempt * 2)
    raise last


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", choices=sorted(SOURCES))
    ap.add_argument("--bbox", help="west,south,east,north in degrees")
    ap.add_argument("--center", help="lat,lon; use with --radius")
    ap.add_argument("--radius", type=float, default=25.0, help="km around --center")
    ap.add_argument("--zooms", default="11-12", help="e.g. 13-15 or 11")
    ap.add_argument("--style", help="basemap only: grau (default) or farbe")
    ap.add_argument("-o", "--out", help="output .mbtiles")
    ap.add_argument("--name", help="metadata name")
    ap.add_argument("--append", action="store_true",
                    help="add these zooms to an existing file instead of replacing it")
    ap.add_argument("--yes", action="store_true", help="skip the size confirmation")
    args = ap.parse_args(argv)

    if not args.bbox and not args.center:
        ap.error("give either --bbox or --center")

    source = dict(SOURCES[args.source])
    if args.style:
        source["style"] = args.style
    zooms = parse_zooms(args.zooms)
    bbox = bbox_from_args(args)
    tiles = tile_list(bbox, zooms)
    out = args.out or ("%s.mbtiles" % args.source)
    name = args.name or "PixelPilot %s" % args.source

    # Measured: the raster sources run 50-70 KB a tile, orthophoto JPEGs about 20. Worth
    # showing, because a zoom level chosen without thinking is the difference between 26 MB
    # and 5 GB - each level is four times the tiles.
    per_tile_kb = 20 if source.get("wms") else 60
    print("%s: %d tiles over zooms %s, roughly %.0f MB"
          % (args.source, len(tiles), args.zooms, len(tiles) * per_tile_kb / 1024.0))
    print("bbox %.4f,%.4f,%.4f,%.4f -> %s" % (bbox[0], bbox[1], bbox[2], bbox[3], out))
    if not args.yes and len(tiles) > 20000:
        print("That is a lot of requests to a free public service. Pass --yes if you mean it.")
        return 1

    db = open_db(out, source, bbox, zooms, name, args.append)
    work = queue.Queue()
    wms = source.get("wms") is not None
    if wms:
        # Whole blocks, deduplicated: several wanted tiles usually fall in the same block.
        blocks = sorted({(z, x - x % BLOCK, y - y % BLOCK) for z, x, y in tiles})
        for b in blocks:
            work.put(b)
        print("  %d tiles fall into %d WMS requests" % (len(tiles), len(blocks)))
    else:
        for t in tiles:
            work.put(t)
    results = queue.Queue()
    failures = []
    wanted = set((z, x, y) for z, x, y in tiles)

    def worker():
        while True:
            try:
                z, x, y = work.get_nowait()
            except queue.Empty:
                return
            if wms:
                try:
                    got = fetch_block(source, z, x, y, BLOCK)
                except SystemExit:
                    raise
                except Exception as e:
                    failures.append((z, x, y, str(e)))
                    got = {}
                # Only the tiles actually asked for; a block overhangs the wanted area.
                for (tx, ty), blob in got.items():
                    if (z, tx, ty) in wanted:
                        results.put((z, tx, ty, blob))
                results.put(("block-done", z, x, y))
            else:
                url = source["url"].format(z=z, x=x, y=y, style=source["style"])
                try:
                    blob = fetch(url)
                except Exception as e:
                    failures.append((z, x, y, str(e)))
                    blob = None
                results.put((z, x, y, blob))
            time.sleep(PAUSE_S)

    threads = [threading.Thread(target=worker, daemon=True)
               for _ in range(WMS_THREADS if wms else THREADS)]
    for t in threads:
        t.start()

    done = 0
    stored = 0
    started = time.time()
    # For a WMS the unit of progress is a request, not a tile - a block yields many tiles at
    # once and some of them are nodata that is never stored.
    total = len({(z, x - x % BLOCK, y - y % BLOCK) for z, x, y in tiles}) if wms else len(tiles)
    while done < total:
        first, z, x, y = (None, None, None, None)
        item = results.get()
        if item[0] == "block-done":
            _, z, x, y = item
            done += 1
            if done % 10 == 0 or done == total:
                db.commit()
                rate = done / max(0.001, time.time() - started)
                print("  %d/%d requests  %.1f/s  eta %.0f s  %d stored  %d failed"
                      % (done, total, rate, (total - done) / max(rate, 0.001), stored,
                         len(failures)), flush=True)
            continue
        z, x, y, blob = item
        if not wms:
            done += 1
        if blob:
            # MBTiles counts tile_row from the bottom (TMS) and the formulas above from the
            # top, which is the single easiest thing to get wrong here.
            db.execute("INSERT OR REPLACE INTO tiles VALUES (?, ?, ?, ?)",
                       (z, x, (1 << z) - 1 - y, sqlite3.Binary(blob)))
            stored += 1
        if not wms and (done % 200 == 0 or done == total):
            db.commit()
            rate = done / max(0.001, time.time() - started)
            print("  %d/%d  %.0f tiles/s  eta %.0f s  %d failed"
                  % (done, total, rate, (total - done) / max(rate, 0.001),
                     len(failures)), flush=True)

    db.commit()
    db.execute("VACUUM")
    db.close()
    print("%s: %d tiles, %.1f MB" % (out, stored, os.path.getsize(out) / 1048576.0))
    if failures:
        # Named rather than counted: a hole in the middle of where you fly matters, and a
        # rerun of the same command fills it.
        print("%d tiles failed, first few:" % len(failures))
        for f in failures[:5]:
            print("   z%d/%d/%d %s" % f)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
