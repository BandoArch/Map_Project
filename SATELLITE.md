# Offline Thika Satellite View

Tap SAT to display satellite coverage and MAP to return to the OSM map.
Drag within the map to pan. Zoom levels 14-18 are available; at the overview
level the whole downloaded area is visible, so panning is constrained.
Both layers cover the existing Thika map area, not all of Kenya.

Imagery: Sentinel-2 cloudless 2016 by EOX IT Services GmbH.
Contains modified Copernicus Sentinel data 2016/2017.
Source: https://s2maps.eu / https://maps.eox.at
License: CC BY 4.0, https://creativecommons.org/licenses/by/4.0/
License statement: https://eox.at/2025/03/sentinel-2-cloudless-2024/
Changes: cropped to Thika, resampled to 512x512, converted to RGB565.
This is historical satellite imagery, not current or street-level photography.

Install Pillow in the host Python environment, then prepare and upload:

```sh
python tools/prepare_satellite.py /tmp/thika-satellite.t565
python tools/upload_map.py --satellite /tmp/thika-satellite.t565
```

The firmware stores the image at `/satellite.t565` on the SD card, separately
from `/map.osm`. USB uploads are framed and checksummed and are read back from
SD before installation. No filesystem formatting is performed. Removing the
card clears both RAM caches; reinsertion reloads the selected layer.
