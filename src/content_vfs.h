// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "filesys.h"
#include "irrlichttypes.h"

/*
	ContentVFS — read-only overlay filesystem for mounted content packs
	(.kpk containers, VOPI Engine).

	A pack carries a mount prefix ("share:/games/foo") resolved against
	porting::path_share / path_user at mount time. The central fs::
	functions (ReadFile, PathExists, IsFile, IsDir, GetDirListing,
	AbsolutePath) consult the overlay, so all existing path arithmetic in
	the engine works unchanged on packed content.

	Resolution rule: THE REAL FILESYSTEM WINS. A loose file at the same
	path shadows the pack entry — this is what makes dev overlays work
	(drop plain files over a mounted pack) and keeps every fallthrough
	trivially safe. Pack entries only fill the gaps the real tree misses.

	Sources and priority. Packs come from two places: the bundled directory
	(<path_share>/packs, shipped with the application) and the installed
	directory (downloaded content, see setInstalledPacksDir). Only "content"
	packs may be mounted from the installed directory — code (bytecode in
	"base" packs) ships exclusively with the application. When two mounted
	packs share an id, the higher version wins and the other is ignored;
	when different packs provide the same path, the lookup order is
	installed before bundled, then higher version, then id — deterministic,
	but a collision is a packaging bug and is logged.

	Container format: tools/kpk/FORMAT.md (normative). This reader
	supports format v1, plain and AES-256-CTR encrypted packs.

	Keys never live in this (public) engine code: an encrypted pack names
	a key_version, and the reader asks the application-installed
	ContentKeyProvider for that key. Without a provider, or when the
	provider does not know the version, the mount is refused. The
	provider is supplied by the proprietary platform layer.

	Thread safety: the mount table is an immutable snapshot behind a
	shared_ptr; readers take their own reference and never observe a
	partially updated table. Writers (startup mount, syncInstalledPacks)
	replace the snapshot atomically and only ever run on the main thread
	while no world is active. Entry reads are stateless (pread), so no
	locks are needed on POSIX. (A mutex guards reads on WIN32 where pread
	is unavailable.)
*/

/*
	Resolves a pack key_version to a 32-byte AES-256 key. Returns false when
	the version is unknown. Installed once at startup by the platform layer
	(see ContentVFS::setKeyProvider); the engine never embeds key material.
*/
typedef bool (*ContentKeyProvider)(u32 key_version, unsigned char key_out[32]);

/*
	Startup hooks the engine calls right before mounting packs. The default
	(weak) definitions in content_vfs.cpp do nothing, so this public code
	carries no keys and no platform paths; the proprietary platform layer
	provides strong definitions. Same mechanism on every platform — no
	per-OS wiring in main().

	vopi_install_content_key_provider — installs the ContentKeyProvider.
	vopi_configure_content_vfs        — sets the installed packs directory
	                                    and the application version
	                                    (setInstalledPacksDir, setAppVersion).
*/
extern "C" void vopi_install_content_key_provider();
extern "C" void vopi_configure_content_vfs();

class ContentPack
{
public:
	// Entry flags (FORMAT.md §5)
	static constexpr u32 EFLAG_ZSTD = 1 << 0;
	static constexpr u32 EFLAG_LUA_BYTECODE = 1 << 1;

	struct Entry {
		u64 offset;
		u64 stored_size;
		u64 raw_size;
		u32 flags;
		std::string sha1_hex; // of raw content; matches engine media hashing
		unsigned char nonce[16]; // AES-CTR initial counter block (encrypted packs)
	};

	// Identity of the file a pack was opened from, used to notice
	// replacement of an installed pack without re-parsing it.
	struct FileIdentity {
		u64 device = 0;
		u64 inode = 0;
		u64 size = 0;
		s64 mtime = 0;
		bool operator==(const FileIdentity &o) const
		{
			return device == o.device && inode == o.inode &&
					size == o.size && mtime == o.mtime;
		}
		bool operator!=(const FileIdentity &o) const { return !(*this == o); }
	};

	~ContentPack();

	// Opens and fully parses a .kpk file. Returns nullptr and fills `err`
	// on any validation failure (bad magic, unsupported version,
	// encrypted with no key for its key_version, malformed index, ...).
	static std::unique_ptr<ContentPack> open(const std::string &path,
			ContentKeyProvider key_provider, std::string &err);

	// stat()s `path` into `out`; false when the file does not exist or is
	// not a regular file.
	static bool statIdentity(const std::string &path, FileIdentity &out);

	bool isEncrypted() const { return m_encrypted; }
	u32 keyVersion() const { return m_key_version; }

	const std::string &id() const { return m_id; }
	const std::string &type() const { return m_type; }
	const std::string &mountSpec() const { return m_mount_spec; } // e.g. "share:/games/foo"
	const std::string &filePath() const { return m_file_path; }
	const FileIdentity &fileIdentity() const { return m_file_identity; }
	int version() const { return m_version; }
	// Minimal application version allowed to mount this pack ("1.4.0");
	// empty when the pack does not care.
	const std::string &minAppVersion() const { return m_min_app_version; }
	// LuaJIT that produced the pack's bytecode entries; empty when the pack
	// carries no bytecode. Checked against the runtime at mount time.
	const std::string &luajitVersion() const { return m_luajit_version; }

	// Entries keyed by container-relative path ("mods/foo/init.lua"),
	// forward slashes, sorted — map order backs directory iteration.
	const std::map<std::string, Entry> &entries() const { return m_entries; }

	const Entry *findEntry(const std::string &rel_path) const;

	// Reads and decompresses one entry. Thread-safe.
	bool readEntry(const Entry &entry, std::string &out) const;

private:
	ContentPack();

	bool readRaw(u64 offset, u64 size, char *dest) const;

	std::string m_file_path;
	FileIdentity m_file_identity;
	std::string m_id;
	std::string m_type;
	std::string m_mount_spec;
	std::string m_luajit_version;
	std::string m_min_app_version;
	int m_version = 0;

	bool m_encrypted = false;
	u32 m_key_version = 0;
	// Expanded AES key schedule (opaque here; aes_ctr.h stays out of the
	// public header). Only meaningful when m_encrypted.
	struct KeySchedule;
	std::unique_ptr<KeySchedule> m_key;

	std::map<std::string, Entry> m_entries;

	int m_fd = -1;
#ifdef _WIN32
	mutable std::mutex m_read_mutex;
	void *m_file_handle = nullptr;
#endif
};

class ContentVFS
{
public:
	enum class Stat { NotFound, File, Dir };

	// Where a mounted pack came from. Determines what may be mounted
	// (installed: content only) and lookup priority (installed first).
	enum class Source { Bundled, Installed };
	static const char *sourceName(Source s);

	// One mounted pack as seen by readers and the Lua API.
	struct MountInfo {
		Source source;
		std::string prefix; // absolute, normalized, no trailing slash
		std::shared_ptr<const ContentPack> pack;
	};

	static ContentVFS &get();

	/*
		Configuration — all called by the platform layer before the startup
		mount (see the vopi_* hooks above).
	*/

	// Installs the key provider used for encrypted packs. Must be called
	// before mounting; nullptr means encrypted packs are refused.
	void setKeyProvider(ContentKeyProvider provider) { m_key_provider = provider; }

	// Directory the delivery layer installs downloaded packs into. Defaults
	// to <path_user>/packs when never set (desktop, dev builds).
	void setInstalledPacksDir(const std::string &dir);
	std::string installedPacksDir() const;

	// Application (not engine) version, e.g. "1.4.2" from the app bundle.
	// Packs whose minAppVersion is newer are refused. Empty (never set)
	// disables the gate — desktop and dev builds have no app version.
	void setAppVersion(const std::string &version);
	const std::string &appVersion() const { return m_app_version; }

	// Numeric, component-wise version comparison ("1.4.10" > "1.4.9",
	// missing components are 0, non-numeric tails are ignored). Returns
	// <0, 0, >0. The delivery layer implements the same rule natively.
	static int compareVersions(const std::string &a, const std::string &b);

	/*
		Mounting — main thread only.
	*/

	// Startup: mounts <path_share>/packs (bundled) and then the installed
	// packs directory. Called once from main() before anything reads
	// game content — which is also before the log file is open, so the
	// per-pack mount lines only reach stderr; see logMountSummary().
	void mountStartupPacks();

	// One actionstream line per visible mount plus the installed packs
	// directory. main() calls it once the log streams are initialized so
	// debug.txt records what was mounted at startup.
	void logMountSummary() const;

	// Scans `dir` for *.kpk and mounts each as `source` (errors are logged,
	// not fatal). World packs are skipped: they are copied, never mounted.
	void mountPacksFromDir(const std::string &dir, Source source);

	// Mounts one pack file as `source`. Returns false and fills `err` on
	// failure. Refuses: unknown format/key, LuaJIT mismatch, minAppVersion
	// newer than the app, non-"content" packs from Source::Installed, and
	// the very same file mounted twice. A different file with an already
	// mounted id is accepted; the higher version becomes the visible one.
	bool mountPackFile(const std::string &pack_path, Source source, std::string &err);

	// Rescans the installed packs directory: mounts new packs, replaces
	// packs whose file changed (atomic rename by the delivery layer), drops
	// packs whose file vanished. Idempotent and cheap when nothing changed
	// (stat per file). Called on every main-menu session and whenever the
	// delivery layer requests it (see requestResync).
	void syncInstalledPacks();

	// Thread-safe request from the delivery layer: "the installed packs
	// directory changed". The main menu loop consumes it on its next frame
	// and runs syncInstalledPacks() — while a world runs the request simply
	// waits until the menu is back.
	void requestResync() { m_resync_requested.store(true); }
	bool consumeResyncRequest() { return m_resync_requested.exchange(false); }

	// Drops every mount (the overlay goes inactive). Startup mounts again
	// as needed; used by unit tests.
	void unmountAll();

	/*
		Queries — any thread.
	*/

	// Fast gate for the fs:: hooks: false until the first successful mount.
	bool isActive() const { return m_active.load(std::memory_order_acquire); }

	// All queries take engine-style absolute paths (mixed separators and
	// redundant components are tolerated). They only see pack content —
	// callers layer the real filesystem on top per the "real wins" rule.
	Stat statPath(const std::string &path) const;
	bool readFile(const std::string &path, std::string &out) const;

	// Appends to `listing` the children of `path` that packs provide and
	// the listing does not already contain (real entries win). Also
	// surfaces mount points when `path` is an ancestor of a mount prefix.
	void addDirEntries(const std::string &path, std::vector<fs::DirListNode> &listing) const;

	// Lexically normalized absolute path for VFS-covered paths — the
	// fs::AbsolutePath fallback when realpath() fails (no real file).
	// Empty string when the path is not covered by any mount.
	std::string normalizedIfCovered(const std::string &path) const;

	// True if `path` lies at/below a mount prefix and the pack (or an
	// ancestor relationship) knows it. Cheap wrapper around statPath.
	bool covers(const std::string &path) const { return statPath(path) != Stat::NotFound; }

	// The visible (winning) pack for `id`, or nullptr. Keeps the pack alive
	// for the caller even if the table is swapped meanwhile.
	std::shared_ptr<const ContentPack> getPack(const std::string &id) const;

	// Snapshot of the visible mounts in lookup-priority order.
	std::vector<MountInfo> getMounts() const;

	// Version string of the LuaJIT the engine runs ("2.1.1785577137");
	// empty when built without LuaJIT.
	static std::string runtimeLuaJITVersion();

	// True only when `path` resolves to an entry served from a bundled
	// ("base") pack AND no real file shadows it — the sole condition under
	// which precompiled bytecode may be loaded (VFS_DESIGN §5). Version
	// compatibility is enforced earlier, at mount time.
	bool isTrustedCodePath(const std::string &path) const;

private:
	using MountTable = std::vector<MountInfo>;

	// Every successfully opened pack, including ones currently shadowed by
	// a higher version of the same id (kept open so they take over again
	// if the winner is removed). Writers only, under m_write_mutex.
	struct Candidate {
		Source source;
		std::string prefix;
		std::shared_ptr<ContentPack> pack;
	};

	// Resolves "share:/sub" / "user:/sub" against porting paths.
	// Empty result = unsupported spec.
	static std::string resolveMountSpec(const std::string &spec);

	// Normalize an engine path for prefix comparison: forward slashes,
	// no ".", "..", no duplicate or trailing separators.
	static std::string normalizePath(const std::string &path);

	// rel = path relative to mount prefix ("" for the prefix itself),
	// or nullopt when path is outside this mount.
	static bool relativeToPrefix(const std::string &norm_path,
			const std::string &prefix, std::string *rel);

	// Validates an opened pack against the mount rules for `source` and
	// resolves its prefix. False + err on refusal.
	bool admitPack(const ContentPack &pack, Source source,
			std::string &prefix, std::string &err) const;

	// Adds a candidate (replacing one opened from the same file path) and
	// republishes the table.
	void addCandidate(Source source, std::string prefix,
			std::shared_ptr<ContentPack> pack);

	// Derives the visible table from m_candidates (dedupe by id, priority
	// order) and publishes it atomically. Logs path collisions between
	// different packs. Caller holds m_write_mutex.
	void republish();

	std::shared_ptr<const MountTable> table() const;

	std::vector<Candidate> m_candidates;
	// Installed files refused by the last sync (with the identity they had),
	// so a broken or foreign file is logged once, not on every menu session.
	std::map<std::string, ContentPack::FileIdentity> m_ignored_installed;
	std::shared_ptr<const MountTable> m_table = std::make_shared<const MountTable>();
	mutable std::mutex m_table_mutex; // guards m_table pointer swaps/reads
	std::mutex m_write_mutex;         // serializes writers
	std::atomic<bool> m_active{false};
	std::atomic<bool> m_resync_requested{false};
	bool m_fetcher_installed = false;
	ContentKeyProvider m_key_provider = nullptr;
	std::string m_installed_dir; // empty = default
	std::string m_app_version;   // empty = no gate
};
