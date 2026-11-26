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
- **`sdk/wapp_stations.c` / `.h` (`wst_*`): every XPRS station on a map and
  in a list**, the portal's map (portal/page/chat_page.html `mapStations`,
  `mapPos`, `mapDraw`, `mapList`, `mapChips`) for wapps. Check-in and Mesh
  link it (`MODULE_SRCS += ../sdk/wapp_stations.c`, `-I../sdk`). Pure
  helpers: `wst_pos_square` (a `pos:` with redaction bars placed in the
  MIDDLE of its square, 6.2.1), `wst_pos_parse`, `wst_dist_m`,
  `wst_dist_text`, `wst_xprs_call`, `wst_base_call`, `wst_ago`,
  `wst_tag_name`. The table `places` in the wapp's own store: `wst_init`
  (options: keep stations with no place, how recent is on the map, marker
  colour, popup actions, skip/placed/done hooks), `wst_follow` (identities and
  service announcements from anyone), `wst_begin`/`wst_step` (the archive
  walked a few pages a tick, newest first, `since:` an hour before the newest
  held, a whole walk every six hours), `wst_heard` (what this device hears
  now), `wst_named` (what `hal_xprs_names` found). The map: `wst_draw` /
  `wst_draw_f(filter)` (one marker per station, the freshest 1500, with
  `kind` archive/person/station and the actions Visit (an archiver) or
  Open, and Message; the host groups and fans them out) and `wst_status`. The list: `wst_filter_t` with `q`, `area` and tags
  (`serve:archive`, `source:solar`, `reach:local`, `placed`, `key`,
  `kind:people`, `kind:other`: the portal's All/Archivers/People/Other),
  `wst_list`, `wst_count`, `wst_chips` (the tags with their counts, as a
  `$type:"chips"` value), `wst_row`, `wst_row_tags`. Any wapp that maps or
  lists stations uses it; a new kind of station is a new tag, no code.
- **A map with its list in a drawer** (`"drawer": true` on the map group, a
  `$type:"people"` field on the same screen, `"search": true` on the screen
  for the search box, a `$type:"chips"` field for filters): the host draws
  the portal's layout, a bottom drawer on a phone and a side panel on a wide
  screen. Mesh's Stations tab is the example. The wapp moves it with
  `ui.drawer` {state: peek|half|full}; `ui.map.locate` folds it and takes
  an optional `zoom`.
- **Marker `actions`** (`[{"id":"open","label":"Open"}]` on a
  `ui.map.marker`): buttons in the marker's popup; a press sends
  `marker_action` {id, action}. Mesh: Open and Message on a station, Show
  all N here on a square.
- **`ui.map.status` items with `"on": false`**: a plain label pill on the
  map (a count, a time) that stays off the app bar's channel chips, which
  show only items that are on. Shown on a map with `"chat": false` too
  (top left, under the search box if there is one).
