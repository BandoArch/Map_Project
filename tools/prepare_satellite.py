"""Download the Thika satellite coverage and convert it to the board's RGB565 format.

Requires Pillow. Source: https://maps.eox.at/, Sentinel-2 cloudless 2016,
EOX IT Services GmbH, modified Copernicus Sentinel data 2016/2017, CC BY 4.0.
"""

import argparse
import io
import math
from pathlib import Path
import struct
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    # Match the OSM renderer's square coverage, including its eight-pixel margin.
    lat, lon = (-1.05126 - 1.03553) / 2, (37.04403 + 37.0647) / 2
    cosine = math.cos(math.radians(lat))
    span = max(0.02067 * cosine, 0.01573) * 1024 / 1008
    bbox = (lon - span / cosine / 2, lat - span / 2,
            lon + span / cosine / 2, lat + span / 2)
    params = dict(service="WMS", request="GetMap", version="1.1.1",
                  layers="s2cloudless", styles="", format="image/jpeg",
                  srs="EPSG:4326", bbox=",".join(map(str, bbox)), width=512, height=512)
    url = "https://a.tiles.maps.eox.at/wms?" + urlencode(params)
    request = Request(url, headers={"User-Agent": "Map_Project offline Thika coverage"})
    with urlopen(request, timeout=60) as response:
        image = Image.open(io.BytesIO(response.read())).convert("RGB")
    if image.size != (512, 512):
        raise ValueError("Unexpected satellite coverage dimensions")
    pixels = bytearray(b"T565\x00\x02\x00\x02")
    rgb = image.tobytes()
    for offset in range(0, len(rgb), 3):
        r, g, b = rgb[offset:offset + 3]
        pixels.extend(struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)))
    args.output.write_bytes(pixels)
    args.output.with_suffix(".license.txt").write_text(
        "Sentinel-2 cloudless 2016 by EOX IT Services GmbH.\n"
        "Contains modified Copernicus Sentinel data 2016/2017.\n"
        "https://s2maps.eu / https://maps.eox.at\n"
        "CC BY 4.0: https://creativecommons.org/licenses/by/4.0/\n"
        "Changes: Thika crop, resampling, RGB565 conversion.\n" + url + "\n",
        encoding="ascii")
    print(f"Prepared {args.output}: {len(pixels)} bytes")


if __name__ == "__main__":
    main()
