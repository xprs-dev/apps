# Reusable pieces in the wapps

- **`sdk/wapp_json.h`**: a growable string (`sb_t`, `sb_puts`, `sb_json`),
  a small JSON reader (`j_get`, `j_str`, `j_num`) and `http_take` for a
  finished `hal_http_request2`. All `static inline`. Lifted out of the AI
  add-on; include it instead of writing another JSON walker.
- **`sdk/wapp_page_meta.h`**: what a web page says about itself (title,
  summary, site, picture; Open Graph, Twitter, JSON-LD, `<title>`), plus
  `url_is_public_http`, `url_resolve`, `url_host` and `pm_image_ext` (a
  picture's type by its first bytes). Pure, tested on
  `linkcard/tests/fixtures`.
- **`hal_media_put_bytes`**: bytes a wapp holds (a picture it downloaded)
  into the media store as a token, with the same preview a picked file gets.
  No temporary file needed.
- **`sdk/wapp_ai.h`**: asking the person's AI from any wapp (`ai.chat`).
- **`fw_ver_cmp` in `firmwares/wire.c`**: orders firmware versions the way
  people mean them (0.10.0 after 0.9.1, 0.6.0-beta.9 before 0.6.0). Lift it
  into the SDK the next time another wapp compares versions.
- **`det_begin` / `det_section` / `det_row` / `det_end` in
  `firmwares/main.c`**: a `$type:"details"` field as titled sections of
  label/value rows, leaving out a row with no value instead of printing "?".
  The shape every "what is known about X" screen wants; candidate for the SDK.
- **`ui.callsign.pick` {field, prefix, length}**: any wapp can ask the
  person to choose a callsign. The app opens its one picker
  (`CallsignPickerPage`) and answers with the command `<field>_picked`
  carrying `<field>_call`, and for X1..X4 the secret `<field>_nsec`. For a
  group (X5) the key stays in the core: create with
  `hal_xprs_group_create_as` by the callsign. In a `ui.prompt` whose `cycle`
  offers group callsigns, `"search": "X5"` adds the same picker beside the
  arrows (Chat's new group).
- **`screen_closed`**: the host sends it when the back arrow or the back
  gesture closes a panel. A wapp with nested panels reopens the parent there
  (Firmwares: WiFi -> Station -> list) instead of adding a Back button.
