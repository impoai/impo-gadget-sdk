# Agent-controlled canvas

The firmware declares `capabilities.screen` and five drawing commands in
`commands_v2`. An agent discovers them with `impo_list_gadgets` and invokes them
through `impo_gadget_command`. Nothing in firmware decides the page layout.
`main/display_canvas.c` owns geometry, text and command validation. X4 supplies
the pixel writer, framebuffer lock and e-paper refresh through a backend.

Coordinates are pixels on a portrait **480×800** canvas. These are command/params
pairs; the caller supplies its account-owned gadget ID through the normal SDK.
Run in order, await each result, and retry `busy` only after the current refresh
or download completes:

```json
[
  {"command":"display.get_info","params":{}},
  {"command":"display.clear","params":{}},
  {"command":"display.draw_text","params":{
    "x":24,"y":24,"width":432,"height":56,
    "text":"MY CANVAS","scale":5}},
  {"command":"display.draw_url","params":{
    "url":"http://YOUR-REACHABLE-HOST/picture.rgb565",
    "x":24,"y":100,"width":432,"height":400,"refresh":false}},
  {"command":"display.draw_text","params":{
    "x":24,"y":530,"width":432,"height":200,
    "text":"Images and text can go anywhere.\nThe agent owns this layout.",
    "scale":3,"background":"white"}},
  {"command":"display.refresh","params":{}},
  {"command":"display.get_info","params":{}}
]
```

The image source in this example is exactly 432×400 pixels. The URL must be
reachable from the gadget's Wi-Fi network. A local HTTP server is suitable for
a bench test; publish only the image directory, never configuration or backups.
HTTPS works when sufficient contiguous RAM is available; low-memory failures
are reported. There is no fixed header, image area or footer on a canvas page.

Use a **direct download URL**, not an image viewer, login or sharing page.
Before sending it, download it on the Agent host and check that the response
is HTTP 200 with image bytes. Only RGB565 and 8-bit baseline, three-component
YCbCr JPEG are supported. PNG, WebP, GIF, progressive JPEG, grayscale JPEG
(one component) and CMYK JPEG must be converted first. A black/white picture
still needs RGB JPEG encoding. Do not retry an unchanged URL after `invalid_image`.

The helper prepares both raw and JPEG input without requiring an image proxy:

```sh
uv run --with pillow python tools/canvas_asset.py photo.png photo.jpg \
  --width 480 --height 800
```

When preparing images directly in Python, use
`im.convert('RGB').save('out.jpg', format='JPEG', progressive=False, subsampling=2)`.
The helper also handles EXIF orientation, resizing and white margins. Publish
the resulting file at a device-reachable URL. Raw RGB565 avoids JPEG artifacts
and is useful for already-dithered pictures or small text.

To update only the bottom text, call `display.clear` with its rectangle,
`display.draw_text` in that rectangle, and `display.refresh`. The upper image
remains in the framebuffer. The whole physical panel still flashes during
refresh. Poll `display.get_info.busy` until false to know when drawing may resume.

On the firmware control transport, successful replies put command data under
`payload`. For example, selected fields from `display.get_info` are
`{"ok":true,"payload":{"width":480,"height":800,"busy":false}}`.
Clear/text replies include `buffered` and
`queued`; refresh returns `queued:true`. A queued refresh is accepted work,
not proof that the physical panel has finished. The Agent SDK may unwrap
`payload` when presenting the result.

## Power-off image

After drawing a page successfully and waiting for `display.get_info.busy:false`,
save it with:

```json
{"command":"display.set_power_off_image","params":{}}
```

This captures the **entire current canvas**, including image and text regions.
There is no URL parameter. It does not turn the device off. The result has
`stored:true`, `persistent:true`, `source:"canvas"`, `width:480`, `height:800`
only after flash write/readback succeeds. The saved image survives reboot and
later canvas changes; shutdown shows it without a network download. If no image
is saved or it cannot be read, the firmware shows the default goodbye page.

To restore that default, use `{"command":"display.set_power_off_image",
"params":{"reset":true}}`; success returns `stored:false, source:"default"`.
Query `display.get_info.power_off_image.stored` after reboot to verify persistence.
Save only when the desired power-off picture changes; this writes flash.

## Chinese, formulas and fonts

The built-in font is ASCII. Agents can render any text or formula into an image
on their own host and send that rectangle through the same image command. This
does not require a new firmware command or a large font in the C3's RAM.

The helper below converts an image into an exact-sized big-endian RGB565 asset,
or renders Unicode text using a caller-selected TrueType/OpenType font. Pillow
is its only extra dependency:

```sh
uv run --with pillow python tools/canvas_asset.py picture.png picture.rgb565 \
  --width 432 --height 400
uv run --with pillow python tools/canvas_asset.py --text 'Hello 世界' \
  --font /path/to/CJK-font.ttf --font-size 36 \
  --width 432 --height 80 --output caption.rgb565
```

Use a font containing the requested glyphs. Mathematical renderers, browsers or
chart libraries can produce the input image for more complex notation. The
helper saves a PNG preview beside the raw asset. It does not upload files,
start a server, or send device commands.
