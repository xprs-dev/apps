/*
 * wapp_i18n.h — the one translation helper for a wapp's C code.
 *
 * Screens translate themselves: a `"@key"` in a .ui.json is resolved by the
 * host from lang/<code>.json. Words the C side composes at runtime -- a room
 * title, a system note, a prompt's title and body, a toast -- do not pass
 * through the screen parser, so the C asks the host for them here:
 *
 *     notify("warning", wtr("msg.could_not_send", "Could not send"));
 *
 * `key` is looked up with hal_i18n_get against the same lang/ files (exact
 * tag → language → en.json); when no file has it the English `en` is returned
 * untouched, so a wapp with no lang/ folder behaves exactly as before and a
 * key added to the code before its translation lands shows English, never a
 * bare key. The English literal therefore stays in the code as the
 * documentation of what the key says, and lang/en.json repeats it verbatim.
 *
 * The answer lives in one of four static buffers in rotation, so up to four
 * wtr() results may be alive in one expression (a printf, a few s_cat calls).
 * A fifth call reuses the first buffer: copy out anything you keep longer.
 * Not thread-safe; a wapp has one thread.
 *
 * Include AFTER the HAL header (it relies on hal_i18n_get being declared and
 * adds no include of its own, because wapps reach the HAL by different
 * relative paths). Not translated through here, ever: wire fields, packet
 * types, callsigns, ids, file names, hal_log lines.
 */
#ifndef WAPP_I18N_H
#define WAPP_I18N_H

#ifndef WTR_CAP
#define WTR_CAP 256u
#endif

static inline const char *wtr(const char *key, const char *en) {
  static char ring[4][WTR_CAP];
  static unsigned next;
  unsigned klen = 0;
  while (key[klen]) klen++;
  char *buf = ring[next++ & 3u];
  uint32_t n = hal_i18n_get(key, klen, buf, WTR_CAP - 1u);
  if (n == 0) return en;
  if (n > WTR_CAP - 1u) n = WTR_CAP - 1u;
  buf[n] = 0;
  return buf;
}

#endif /* WAPP_I18N_H */
