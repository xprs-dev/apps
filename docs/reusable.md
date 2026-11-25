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
- **`ci_pos_square` in `checkin/ci.c`**: a `pos:` with redaction bars
  (6.2.1, `38█████,-9█████`) placed in the MIDDLE of the square its readable
  digits name, with the square's half-width in metres; a plain value is its
  own point. The C twin of the core's `xprsPosCell` and the xprs.dev map's
  `parsePos`. Any wapp that maps stations uses it (lift it into the SDK the
  second time). Beside it `ci_xprs_call`: an XPRS callsign, not another
  network's node.
- **Map from a kept copy, brought up to date behind it** (Check-in's
  "Stations everywhere" in `checkin/main.c`): follow a packet type with
  `hal_xprs_follow_kind` so the core keeps it and asks the archivers;
  copy what the screen needs into a small table of the wapp's own; draw from
  that table at once with an "Updated <when>" `ui.map.status` pill; walk the
  archive newest first a couple of pages per wake (`since:` an hour before
  the newest held, a whole walk every few hours). The shape for any screen
  built from a type that arrives from everywhere.
- **`ui.map.status` items with `"on": false`**: a plain label pill on the
  map (a count, a time) that stays off the app bar's channel chips, which
  show only items that are on.
