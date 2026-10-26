/* HangulPad version — the single source of truth.
 *
 * Bump on every change that ships (see CHANGELOG.md, "버전 규칙" in README):
 *   MAJOR  0 until 1.0 (editing UI + HWP read complete); then incompatible changes
 *   MINOR  new feature or changed file-format behaviour
 *   PATCH  bug fixes only
 * Keep HP_VERSION_STRING equal to MAJOR.MINOR.PATCH (checked by tests). */
#ifndef HP_VERSION_H
#define HP_VERSION_H

#define HP_VERSION_MAJOR 0
#define HP_VERSION_MINOR 8
#define HP_VERSION_PATCH 4
#define HP_VERSION_STRING "0.8.4"
#define HP_VERSION_DATE "2026-10-27"

/* "0, 8, 4, 0" — Hangul-style appVersion written into new documents' version.xml */
#define HP_VERSION_COMMAS "0, 8, 4, 0"

#ifndef RC_INVOKED
#ifdef __cplusplus
extern "C" {
#endif
const char *hp_version(void); /* "0.8.4" (for FFI callers) */
#ifdef __cplusplus
}
#endif
#endif

#endif /* HP_VERSION_H */




