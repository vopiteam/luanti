# Security Policy

This repository is **VOPI Engine**, a fork of Luanti. Where to report depends on
which code is affected.

## Upstream engine code

If the issue reproduces on unmodified upstream Luanti, report it upstream —
they maintain that code and issue the fixes:
<https://github.com/luanti-org/luanti/security>

## Fork-specific code

For anything in the fork's own additions — the mobile platform layer, the
content VFS, the UI and touch changes, anything behind `IS_VOPI_ENGINE` — open
a **private security advisory** on this repository:

> Security → Advisories → Report a vulnerability

That keeps the report private until a fix ships. Please do not open a public
issue for a vulnerability, and do not email upstream's maintainers about
fork-specific code: they cannot fix it.

## Supported versions

Only the current fork revision is supported — see the version string reported by
the build (`5.16.1-ve1` at the time of writing). Older revisions receive no
fixes.
