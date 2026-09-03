#!/usr/bin/env python3
"""Builds the offline MBTiles the headset HUD reads: a basemap and a terrain height model.

The immersive HUD's minimap and height profile work with no connection, which is the situation
the app is flown in - so the tiles have to be on the device beforehand. This produces both
files from sources that may actually be redistributed:

  basemap   basemap.de Web Raster, published by the BKG for the AdV under Datenlizenz
            Deutschland - Namensnennung 2.0. Germany only, but genuinely open, which the
            usual OpenStreetMap tile servers are not: their tile usage policy rules out bulk
            downloading, so they are not an option here however convenient they look.

  terrain   The Terrain Tiles open dataset on AWS, terrarium encoding. Global, no account.

Both licences require naming the source. The attribution string goes into the MBTiles metadata
and the HUD renders it on the minimap, so a built file carries its own credit.

Usage, for the whole of Baden-Wuerttemberg plus detail around a flying site:

    python scripts/build_offline_maps.py basemap --bbox 7.4,47.4,10.6,49.9 --zooms 11-12
    python scripts/build_offline_maps.py basemap --center 48.78,9.18 --radius 25 --zooms 13-15
    python scripts/build_offline_maps.py terrain --bbox 7.4,47.4,10.6,49.9 --zooms 11

Zoom is what decides size, and it decides it steeply - each level is four times the tiles:

    z11   51 m/px      z13   13 m/px      z15   3.2 m/px
    z12   25 m/px      z14  6.3 m/px

The minimap picks the closest zoom the file has for the span it is drawing, which is
max(240 m, 2.6x the distance from home) across 512 px. So a statewide z11-z12 file is right for
orientation and long flights, and z14-z15 within a radius of where you actually fly is what
makes it sharp close in. The height profile is not fussy - z11 covers a state in 26 MB and
terrain does not change between zoom levels the way a map's detail does.

Copy the results to the headset with:

    adb push terrain-bw.mbtiles basemap-bw.mbtiles /sdcard/Download/

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

# Four at a time with a short pause. These are public services doing us a favour; the job is
# minutes either way, and there is no reason to make it look like an attack.
THREADS = 4
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


def make_db(path, source, bbox, zooms, name):
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

    # Guessed from measurement: these sources run 50-70 KB a tile. Worth showing, because a
    # zoom level chosen without thinking is the difference between 26 MB and 5 GB.
    print("%s: %d tiles over zooms %s, roughly %.0f MB"
          % (args.source, len(tiles), args.zooms, len(tiles) * 60 / 1024.0))
    print("bbox %.4f,%.4f,%.4f,%.4f -> %s" % (bbox[0], bbox[1], bbox[2], bbox[3], out))
    if not args.yes and len(tiles) > 20000:
        print("That is a lot of requests to a free public service. Pass --yes if you mean it.")
        return 1

    db = make_db(out, source, bbox, zooms, name)
    work = queue.Queue()
    for t in tiles:
        work.put(t)
    results = queue.Queue()
    failures = []

    def worker():
        while True:
            try:
                z, x, y = work.get_nowait()
            except queue.Empty:
                return
            url = source["url"].format(z=z, x=x, y=y, style=source["style"])
            try:
                blob = fetch(url)
            except Exception as e:
                failures.append((z, x, y, str(e)))
                blob = None
            results.put((z, x, y, blob))
            time.sleep(PAUSE_S)

    threads = [threading.Thread(target=worker, daemon=True) for _ in range(THREADS)]
    for t in threads:
        t.start()

    done = 0
    stored = 0
    started = time.time()
    while done < len(tiles):
        z, x, y, blob = results.get()
        done += 1
        if blob:
            # MBTiles counts tile_row from the bottom (TMS) and the formulas above from the
            # top, which is the single easiest thing to get wrong here.
            db.execute("INSERT OR REPLACE INTO tiles VALUES (?, ?, ?, ?)",
                       (z, x, (1 << z) - 1 - y, sqlite3.Binary(blob)))
            stored += 1
        if done % 200 == 0 or done == len(tiles):
            db.commit()
            rate = done / max(0.001, time.time() - started)
            print("  %d/%d  %.0f tiles/s  eta %.0f s  %d failed"
                  % (done, len(tiles), rate, (len(tiles) - done) / max(rate, 0.001),
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
