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
