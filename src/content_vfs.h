// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#pragma once

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

	Container format: tools/kpk/FORMAT.md (normative). This reader
	supports format v1, unencrypted packs; the ENCRYPTED flag is refused
	until key support lands (a later migration stage).

	Thread safety: mounting happens once during early startup, before
	worker threads exist. After that the mount table is immutable and all
	read paths are stateless (pread), so concurrent access needs no locks
	on POSIX. (A mutex guards reads on WIN32 where pread is unavailable.)
*/

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
	};

	~ContentPack();

	// Opens and fully parses a .kpk file. Returns nullptr and fills `err`
	// on any validation failure (bad magic, unsupported version,
	// encrypted without key support, malformed index, ...).
	static std::unique_ptr<ContentPack> open(const std::string &path, std::string &err);

	const std::string &id() const { return m_id; }
	const std::string &type() const { return m_type; }
	const std::string &mountSpec() const { return m_mount_spec; } // e.g. "share:/games/foo"
	const std::string &filePath() const { return m_file_path; }
	int version() const { return m_version; }
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
	ContentPack() = default;

	bool readRaw(u64 offset, u64 size, char *dest) const;

	std::string m_file_path;
	std::string m_id;
	std::string m_type;
	std::string m_mount_spec;
	std::string m_luajit_version;
	int m_version = 0;

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

	static ContentVFS &get();

	// Scans `dir` for *.kpk and mounts each (errors are logged, not fatal).
	// Called from early startup for path_share/packs and path_user/packs.
	void mountPacksFromDir(const std::string &dir);

	// Mounts one pack file. Returns false and fills `err` on failure.
	bool mountPackFile(const std::string &pack_path, std::string &err);

	// Fast gate for the fs:: hooks: false until the first successful mount.
	bool isActive() const { return m_active; }

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

	const ContentPack *getPack(const std::string &id) const;

	// Version string of the LuaJIT the engine runs ("2.1.1785577137");
	// empty when built without LuaJIT.
	static std::string runtimeLuaJITVersion();

	// True only when `path` resolves to an entry served from a bundled
	// ("base") pack AND no real file shadows it — the sole condition under
	// which precompiled bytecode may be loaded (VFS_DESIGN §5). Version
	// compatibility is enforced earlier, at mount time.
	bool isTrustedCodePath(const std::string &path) const;

	// Drops every mount (the overlay goes inactive). Startup mounts again
	// as needed; used by unit tests and future dynamic remounting.
	void unmountAll();

private:
	struct Mount {
		std::string prefix; // absolute, normalized, no trailing slash
		std::unique_ptr<ContentPack> pack;
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

	std::vector<Mount> m_mounts;
	bool m_active = false;
};
