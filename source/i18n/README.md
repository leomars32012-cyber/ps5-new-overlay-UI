# Notification strings

Notification text lives in one JSON file per locale and is compiled into
the ELF by `generate_catalog.py`. The console does not load locale files at
runtime.

There is one locale file per PS5 console language
(`SCE_SYSTEM_SERVICE_PARAM_ID_LANG`), 28 in total. `en-US.json` is the
fallback (`meta.fallback: true`).

## File layout

```json
{
  "meta": { "id": "en", "bcp47": "en-US", "fallback": true },
  "notifications": {
    "notify.brand": "FPS Monitor"
  }
}
```

- `meta.id` is the locale id that `notify_i18n.c` maps each
  `ONION_NOTIFY_LANG_*` value to.
- `notifications` holds `notify.*` keys. The only key in use today is
  `notify.brand`, the default title of a rich notification
  (`onion_notify_rich` in `libonion_platform/source/notify.c`). It is a brand
  token and reads `FPS Monitor` in every locale.

The overlay's own toasts (`[FPS Monitor] started`, `already running`, ...)
are plain English strings passed straight to `onion_notify`. A string that is
not a catalog key is shown unchanged.

## Build

`source/libonion_platform/CMakeLists.txt` runs:

```sh
python3 generate_catalog.py --locales-dir . --catalog notifications \
  --output <build>/generated/notify_i18n_catalog.inc
```

`notify_i18n.c` includes the generated table. The generator rejects:

- a locale whose keys differ from `en-US.json`
- empty keys, non-string values, or embedded NUL
- mismatched `printf` conversions (`%s`, `%d`, ...) between locales
- a `notify.brand` value without the `FPS Monitor` token
- doubled spaces inside a string

## Adding a notification

1. Add a stable `notify.<area>.<name>` key to every locale file. Keep the
   `printf` placeholders identical in every language.
2. Pass the key, not the English text:

   ```c
   onion_notify(0, "notify.overlay.load_failed");
   ```

3. Rebuild. A key missing from any locale fails the catalog step.

Logs (`LOG_*`) are developer-facing and stay English.
