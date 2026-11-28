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
- **`dv_ver_cmp` in `devices/wire.c`**: orders firmware versions the way
  people mean them (0.10.0 after 0.9.1, 0.6.0-beta.9 before 0.6.0). Lift it
  into the SDK the next time another wapp compares versions.
- **`det_begin` / `det_section` / `det_row` / `det_end` in
  `devices/stations.c`**: a `$type:"details"` field as titled sections of
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
  (Devices: WiFi -> Station -> list) instead of adding a Back button.
- **Two wapps in one, `devices/main.c` + `devices.h`**: how Firmwares and
  Things became one wapp without rewriting either. A wapp has one inbox and
  one event queue, so a small shell reads both and hands each command and
  event to the half it belongs to; `dv_sub(who, topic, want)` holds a
  subscription while either half wants it (one half letting go must not
  silence the other); each half writes its list sections with no envelope
  (`st_sections`, `th_sections`) and the shell draws the one list. Reuse the
  shape the next time two wapps merge.
- **Testing one half of a merged wapp, `devices/tests/native/`**: the real
  shell and one real half are linked against a stub of the other half
  (`stub_things.c`, `stub_stations.c`), so each suite keeps its own mock HAL.
- **`devices/wire.c` (`dv_*`)**: the one copy of the no-libc strings, wire
  field reader, small JSON reader, base64 and the 11.9/11.10 command
  builders that Firmwares and Things each carried. Candidate for the SDK.
- **`hal_identity_device`**: this device's own station callsign (its `X3`),
  for a wapp that lists or administers the device it runs on.
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
  `kind` archive/person/station, `tags`, and the actions Visit (every
  station), Message (a key) and Keep my messages here (an archiver not yet
  keeping them; `wst_mine` takes `hal_xprs_archivers`' answer, `wst_is_mine`
  asks); the host groups and fans them out), `wst_kinds` (the filter over
  the map, `ui.map.filter`, with counts) and `wst_status`. The list: `wst_filter_t` with `q`, `area` and tags
  (`serve:archive`, `source:solar`, `reach:local`, `placed`, `key`,
  and `kind` "", "archive", "people", "other": the portal's
  All/Archivers/People/Other),
  `wst_list`, `wst_count`, `wst_chips` (the tags with their counts, as a
  `$type:"chips"` value), `wst_row`, `wst_row_tags`. Any wapp that maps or
  lists stations uses it; a new kind of station is a new tag, no code.
- **Time in a station's range, from the core** (Check-in, `ci_visit_step` /
  `ci_span` in `checkin/ci.c`, `visits_sample` in `main.c`): sample
  `hal_xprs_stations` on the wapp's tick, take `now - heard.ble` as when the
  station was last heard, and let the pure step open, extend or close a visit
  (gap 10 min, under 2 min is passing by); `ci_span` writes "3 h 20 min",
  "2 d 4 h", "3 wk 2 d". Any wapp that measures presence (a car, a club, a
  family member) reuses both.
- **Tags on records in a zx store** (Check-in `spots.tags`, `mytags`,
  `each_tag`, `ftag_toggle`): a row keeps its tags as ",a,b," so a filter is
  `tags LIKE '%,a,%'` per chosen tag; the chips are every tag with its count
  (`$type:"chips"`), a Tags page lists them with totals. Worth moving to the
  SDK when a second wapp tags things.
- **Where something happened, on a small map** (`$type:"place"`, wapp-interfaces
  §14.0.2): set the field to `{"lat","lon","label"}`; Check-in's `place_set`
  turns a `pos:` into that value. Its `d_begin`/`d_section`/`d_item`/`d_end`
  build a `$type:"details"` value fact by fact, leaving out empty ones;
  `find_event` finds the newest announcement a station made under a title in
  the archive (an event's description, a check-in point's words).
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

- **Moderation in rooms that belong to nobody** (Chat, XPRS.md 10.10.3): the
  three closed-group verbs (`hal_xprs_group_moderation`,
  `hal_xprs_group_role_at`, `hal_xprs_group_moderate`) answer for the group
  `"*"`, the public rooms. The core holds the list and the verdict; the wapp
  folds removals in with the code it has for a closed group
  (`room_moderation_sync`), listens to `core.moderation`, and declares
  `"mod":"remove"` on the room's row so the host offers Delete alone. A wapp
  showing other public publications can do the same.
