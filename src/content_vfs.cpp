// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#include "content_vfs.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <sstream>

#include <json/json.h>
#include <zstd.h>

#include "aes_ctr.h"
#include "config.h"
#include "log.h"
#include "porting.h"

#if USE_LUAJIT
	#include <luajit.h>
#endif

#include <sys/stat.h>
#ifndef _WIN32
	#include <fcntl.h>
	#include <unistd.h>
#endif

#if CHECK_CLIENT_BUILD()
#include <IFileSystem.h>

// Bridges Irrlicht's path-based image loading (base pack textures, menu
// textures, icons) into the overlay: consulted by CFileSystem when the
// native filesystem misses. Buffer ownership passes to Irrlicht (new char[]).
static bool contentVFSIrrFetcher(const char *filename, char **data, long *size)
{
	std::string out;
	if (!ContentVFS::get().readFile(filename, out))
		return false;
	char *buf = new char[out.size()];
	std::memcpy(buf, out.data(), out.size());
	*data = buf;
	*size = (long)out.size();
	return true;
}
#endif

namespace {

constexpr char KPK_MAGIC[4] = {'K', 'P', 'K', '1'};
constexpr u32 KPK_FORMAT_VERSION = 1;
constexpr size_t KPK_HEADER_SIZE = 80;

constexpr u32 FLAG_ENCRYPTED = 1 << 0;
constexpr u32 FLAG_INDEX_ZSTD = 1 << 1;

// Cap for any decompressed object (index or entry): bounds the allocation
// a corrupt or crafted container can force.
constexpr u64 MAX_ENTRY_RAW_SIZE = (u64)1 << 31; // 2 GiB

inline u32 readLEU32(const char *p)
{
	const u8 *b = reinterpret_cast<const u8 *>(p);
	return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}

inline u64 readLEU64(const char *p)
{
	return (u64)readLEU32(p) | ((u64)readLEU32(p + 4) << 32);
}

bool parseJson(const std::string &data, Json::Value *out, std::string *errs)
{
	Json::CharReaderBuilder builder;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	return reader->parse(data.data(), data.data() + data.size(), out, errs);
}

bool zstdDecompress(const char *src, size_t src_size, std::string &out,
		u64 known_raw_size)
{
	size_t raw_size = known_raw_size;
	if (raw_size == 0) {
		unsigned long long content_size =
				ZSTD_getFrameContentSize(src, src_size);
		if (content_size == ZSTD_CONTENTSIZE_UNKNOWN ||
				content_size == ZSTD_CONTENTSIZE_ERROR ||
				content_size > MAX_ENTRY_RAW_SIZE)
			return false;
		raw_size = content_size;
	}
	out.resize(raw_size);
	size_t r = ZSTD_decompress(&out[0], raw_size, src, src_size);
	return !ZSTD_isError(r) && r == raw_size;
}

// 32 lowercase/uppercase hex chars -> 16 bytes; false on any other input.
bool parseHex16(const std::string &hex, unsigned char out[16])
{
	if (hex.size() != 32)
		return false;
	for (size_t i = 0; i < 16; i++) {
		unsigned int v = 0;
		for (int k = 0; k < 2; k++) {
			char c = hex[i * 2 + k];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= c - '0';
			else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
			else return false;
		}
		out[i] = (unsigned char)v;
	}
	return true;
}

// admitPack() refusal for world packs; the directory scanners recognize it
// to skip such files quietly instead of logging an error.
const char *const WORLD_PACK_NOT_MOUNTABLE = "world packs are copied into a world, not mounted";

// A container-relative entry path must stay inside the mount.
bool isSafeEntryPath(const std::string &p)
{
	if (p.empty() || p[0] == '/')
		return false;
	size_t pos = 0;
	while (pos <= p.size()) {
		size_t next = p.find('/', pos);
		if (next == std::string::npos)
			next = p.size();
		std::string_view seg(p.data() + pos, next - pos);
		if (seg.empty() || seg == "." || seg == "..")
			return false;
		pos = next + 1;
	}
	return p.find('\\') == std::string::npos;
}

#ifndef _WIN32
// File identity from a stat buffer, with nanosecond mtime where the
// platform provides it: an installed pack replaced twice within a second
// by files of equal size that reuse the inode number must still register
// as changed.
ContentPack::FileIdentity identityFromStat(const struct stat &st)
{
	ContentPack::FileIdentity id;
	id.device = (u64)st.st_dev;
	id.inode = (u64)st.st_ino;
	id.size = (u64)st.st_size;
#if defined(__APPLE__)
	id.mtime = (s64)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#elif defined(__linux__) || defined(__ANDROID__)
	id.mtime = (s64)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#else
	id.mtime = (s64)st.st_mtime * 1000000000LL;
#endif
	return id;
}
#endif

} // namespace

// Weak defaults: no key provider, no platform configuration. Overridden by
// the platform layer's strong definitions when it is linked in (MSVC has no
// weak symbols; there the platform layer must call the ContentVFS setters
// itself before mount).
#if defined(__GNUC__) || defined(__clang__)
extern "C" __attribute__((weak)) void vopi_install_content_key_provider()
{
}

extern "C" __attribute__((weak)) void vopi_configure_content_vfs()
{
}
#endif

/*
	ContentPack
*/

struct ContentPack::KeySchedule {
	aes256_key ctx;
};

ContentPack::ContentPack() = default;

ContentPack::~ContentPack()
{
	if (m_fd >= 0) {
#ifndef _WIN32
		::close(m_fd);
#endif
	}
#ifdef _WIN32
	if (m_file_handle)
		std::fclose(reinterpret_cast<FILE *>(m_file_handle));
#endif
}

bool ContentPack::readRaw(u64 offset, u64 size, char *dest) const
{
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(m_read_mutex);
	FILE *fp = reinterpret_cast<FILE *>(m_file_handle);
	if (_fseeki64(fp, offset, SEEK_SET) != 0)
		return false;
	return std::fread(dest, 1, size, fp) == size;
#else
	u64 done = 0;
	while (done < size) {
		// Chunked so the size_t argument never narrows on 32-bit targets
		const size_t want = (size_t)std::min<u64>(size - done, (u64)1 << 30);
		ssize_t r = ::pread(m_fd, dest + done, want, (off_t)(offset + done));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		done += (u64)r;
	}
	return true;
#endif
}

std::unique_ptr<ContentPack> ContentPack::open(const std::string &path,
		ContentKeyProvider key_provider, std::string &err)
{
	std::unique_ptr<ContentPack> pack(new ContentPack());
	pack->m_file_path = path;

	u64 file_size;
#ifdef _WIN32
	FILE *fp = std::fopen(path.c_str(), "rb");
	if (!fp) {
		err = "cannot open file";
		return nullptr;
	}
	pack->m_file_handle = fp;
	if (_fseeki64(fp, 0, SEEK_END) != 0) {
		err = "cannot determine file size";
		return nullptr;
	}
	const __int64 end = _ftelli64(fp);
	if (end < 0) {
		err = "cannot determine file size";
		return nullptr;
	}
	file_size = (u64)end;
	statIdentity(path, pack->m_file_identity);
#else
	// fstat rather than fseek/ftell: ftell returns long (32-bit on
	// armeabi-v7a) and -1 on error, which as u64 would defeat every bounds
	// check below. Also rejects directories and other non-regular files.
	pack->m_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (pack->m_fd < 0) {
		err = "cannot open file";
		return nullptr;
	}
	struct stat st{};
	if (::fstat(pack->m_fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
		err = "not a regular file";
		return nullptr;
	}
	file_size = (u64)st.st_size;
	// Identity of the opened inode itself (not a second stat by path):
	// this is what syncInstalledPacks compares against later.
	pack->m_file_identity = identityFromStat(st);
#endif

	char header[KPK_HEADER_SIZE];
	if (file_size < KPK_HEADER_SIZE || !pack->readRaw(0, KPK_HEADER_SIZE, header)) {
		err = "file too small for KPK header";
		return nullptr;
	}
	if (std::memcmp(header, KPK_MAGIC, 4) != 0) {
		err = "not a KPK file (bad magic)";
		return nullptr;
	}

	const u32 format_version = readLEU32(header + 4);
	const u32 flags = readLEU32(header + 8);
	const u32 key_version = readLEU32(header + 12);
	const u64 meta_offset = readLEU64(header + 16);
	const u64 meta_size = readLEU64(header + 24);
	// preview block (header + 32..47) is storefront data — irrelevant here
	const u64 index_offset = readLEU64(header + 48);
	const u64 index_size = readLEU64(header + 56);
	const unsigned char *index_nonce =
			reinterpret_cast<const unsigned char *>(header + 64);

	if (format_version != KPK_FORMAT_VERSION) {
		err = "unsupported KPK format version " + std::to_string(format_version);
		return nullptr;
	}
	if (flags & FLAG_ENCRYPTED) {
		if (!key_provider) {
			err = "pack is encrypted but no content key provider is installed";
			return nullptr;
		}
		unsigned char key[32];
		if (!key_provider(key_version, key)) {
			err = "no key for key_version " + std::to_string(key_version);
			return nullptr;
		}
		pack->m_encrypted = true;
		pack->m_key_version = key_version;
		pack->m_key.reset(new KeySchedule());
		aes256_set_key(&pack->m_key->ctx, key);
		std::memset(key, 0, sizeof(key));
	}
	// Overflow-safe bounds checks: `off + size > file_size` would wrap for
	// crafted u64 values and pass, and the sizes below feed allocations.
	if (meta_offset > file_size || meta_size > file_size - meta_offset ||
			index_offset > file_size || index_size > file_size - index_offset) {
		err = "header blocks out of file bounds";
		return nullptr;
	}

	// Meta
	std::string meta_raw(meta_size, '\0');
	if (!pack->readRaw(meta_offset, meta_size, &meta_raw[0])) {
		err = "cannot read meta block";
		return nullptr;
	}
	Json::Value meta;
	std::string jerr;
	if (!parseJson(meta_raw, &meta, &jerr)) {
		err = "meta parse failed: " + jerr;
		return nullptr;
	}
	if (!meta["id"].isString() || !meta["mount"].isString() ||
			!meta["type"].isString() || !meta["version"].isInt()) {
		err = "meta lacks required fields (id/type/mount/version)";
		return nullptr;
	}
	pack->m_id = meta["id"].asString();
	pack->m_type = meta["type"].asString();
	pack->m_mount_spec = meta["mount"].asString();
	pack->m_version = meta["version"].asInt();
	if (meta["luajitVersion"].isString())
		pack->m_luajit_version = meta["luajitVersion"].asString();
	if (meta["minAppVersion"].isString())
		pack->m_min_app_version = meta["minAppVersion"].asString();

	// Index
	std::string index_stored(index_size, '\0');
	if (!pack->readRaw(index_offset, index_size, &index_stored[0])) {
		err = "cannot read index block";
		return nullptr;
	}
	if (pack->m_encrypted) {
		// FORMAT.md §5: compress, then encrypt — so decrypt first
		aes256_ctr_xor(&pack->m_key->ctx, index_nonce,
				reinterpret_cast<const uint8_t *>(index_stored.data()),
				reinterpret_cast<uint8_t *>(&index_stored[0]),
				index_stored.size());
	}
	std::string index_plain;
	if (flags & FLAG_INDEX_ZSTD) {
		if (!zstdDecompress(index_stored.data(), index_stored.size(),
				index_plain, 0)) {
			err = pack->m_encrypted
					? "index decompression failed (wrong key?)"
					: "index decompression failed";
			return nullptr;
		}
	} else {
		index_plain = std::move(index_stored);
	}

	Json::Value index;
	if (!parseJson(index_plain, &index, &jerr)) {
		err = "index parse failed: " + jerr;
		return nullptr;
	}
	const Json::Value &files = index["files"];
	if (!files.isArray()) {
		err = "index lacks 'files' array";
		return nullptr;
	}

	bool has_bytecode = false;
	for (const Json::Value &f : files) {
		if (!f["p"].isString() || !f["o"].isUInt64() || !f["s"].isUInt64() ||
				!f["r"].isUInt64() || !f["f"].isUInt() || !f["h"].isString()) {
			err = "malformed index entry";
			return nullptr;
		}
		const std::string p = f["p"].asString();
		if (!isSafeEntryPath(p)) {
			err = "unsafe entry path: " + p;
			return nullptr;
		}
		Entry e;
		e.offset = f["o"].asUInt64();
		e.stored_size = f["s"].asUInt64();
		e.raw_size = f["r"].asUInt64();
		e.flags = f["f"].asUInt();
		e.sha1_hex = f["h"].asString();
		std::memset(e.nonce, 0, sizeof(e.nonce));
		if (pack->m_encrypted) {
			if (!f["n"].isString() || !parseHex16(f["n"].asString(), e.nonce)) {
				err = "malformed entry nonce: " + p;
				return nullptr;
			}
		}
		if (e.offset > file_size || e.stored_size > file_size - e.offset) {
			err = "entry out of file bounds: " + p;
			return nullptr;
		}
		// raw_size feeds the decompression allocation; no legitimate single
		// content file approaches this.
		if (e.raw_size > MAX_ENTRY_RAW_SIZE) {
			err = "implausible raw size for entry: " + p;
			return nullptr;
		}
		has_bytecode = has_bytecode || (e.flags & EFLAG_LUA_BYTECODE);
		if (!pack->m_entries.emplace(p, std::move(e)).second) {
			err = "duplicate entry path: " + p;
			return nullptr;
		}
	}

	if (pack->m_entries.empty()) {
		err = "pack contains no entries";
		return nullptr;
	}

	// Bytecode entries without a recorded LuaJIT version cannot be
	// version-gated at mount time — refuse instead of failing later with
	// an opaque chunk error (only the packer omitting the field, i.e. a
	// crafted or corrupt container, produces this).
	if (has_bytecode && pack->m_luajit_version.empty()) {
		err = "pack has bytecode entries but no luajitVersion in meta";
		return nullptr;
	}

	return pack;
}

const ContentPack::Entry *ContentPack::findEntry(const std::string &rel_path) const
{
	auto it = m_entries.find(rel_path);
	return it == m_entries.end() ? nullptr : &it->second;
}

bool ContentPack::readEntry(const Entry &entry, std::string &out) const
{
	if (entry.stored_size == 0) {
		out.clear();
		return entry.raw_size == 0;
	}

	std::string stored(entry.stored_size, '\0');
	if (!readRaw(entry.offset, entry.stored_size, &stored[0]))
		return false;

	if (m_encrypted) {
		aes256_ctr_xor(&m_key->ctx, entry.nonce,
				reinterpret_cast<const uint8_t *>(stored.data()),
				reinterpret_cast<uint8_t *>(&stored[0]), stored.size());
	}

	if (entry.flags & EFLAG_ZSTD)
		return zstdDecompress(stored.data(), stored.size(), out, entry.raw_size);

	out = std::move(stored);
	return out.size() == entry.raw_size;
}

bool ContentPack::statIdentity(const std::string &path, FileIdentity &out)
{
#ifdef _WIN32
	struct _stat64 st{};
	if (_stat64(path.c_str(), &st) != 0 || !(st.st_mode & _S_IFREG))
		return false;
	out.device = (u64)st.st_dev;
	out.inode = (u64)st.st_ino;
	out.size = (u64)st.st_size;
	out.mtime = (s64)st.st_mtime * 1000000000LL;
	return true;
#else
	struct stat st{};
	if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
		return false;
	out = identityFromStat(st);
	return true;
#endif
}

/*
	ContentVFS
*/

ContentVFS &ContentVFS::get()
{
	static ContentVFS instance;
	return instance;
}

const char *ContentVFS::sourceName(Source s)
{
	return s == Source::Installed ? "installed" : "bundled";
}

void ContentVFS::setInstalledPacksDir(const std::string &dir)
{
	m_installed_dir = dir.empty() ? "" : normalizePath(dir);
}

std::string ContentVFS::installedPacksDir() const
{
	if (!m_installed_dir.empty())
		return m_installed_dir;
	return normalizePath(porting::path_user + "/packs");
}

void ContentVFS::setAppVersion(const std::string &version)
{
	m_app_version = version;
}

int ContentVFS::compareVersions(const std::string &a, const std::string &b)
{
	// Component by component; a missing component counts as 0 so
	// "1.4" == "1.4.0". Digits are read until the first non-digit of the
	// component, so a stray suffix ("1.4.2-rc") neither breaks the
	// comparison nor participates in it.
	auto next_component = [](const std::string &s, size_t &pos) -> u64 {
		u64 v = 0;
		bool in_digits = true;
		while (pos < s.size() && s[pos] != '.') {
			const char c = s[pos++];
			if (in_digits && c >= '0' && c <= '9') {
				if (v < ((u64)1 << 40))
					v = v * 10 + (u64)(c - '0');
			} else {
				in_digits = false; // suffix: skipped, not compared
			}
		}
		if (pos < s.size())
			pos++; // the '.'
		return v;
	};

	size_t ia = 0, ib = 0;
	while (ia < a.size() || ib < b.size()) {
		const u64 va = next_component(a, ia);
		const u64 vb = next_component(b, ib);
		if (va != vb)
			return va < vb ? -1 : 1;
	}
	return 0;
}

std::string ContentVFS::normalizePath(const std::string &path)
{
	std::string p = path;
	std::replace(p.begin(), p.end(), '\\', '/');
	const bool absolute = !p.empty() && p[0] == '/';

	std::vector<std::string> parts;
	size_t pos = 0;
	while (pos <= p.size()) {
		size_t next = p.find('/', pos);
		if (next == std::string::npos)
			next = p.size();
		std::string seg = p.substr(pos, next - pos);
		if (seg == "..") {
			if (!parts.empty() && parts.back() != "..")
				parts.pop_back();
			else if (!absolute)
				parts.push_back("..");
		} else if (!seg.empty() && seg != ".") {
			parts.push_back(std::move(seg));
		}
		pos = next + 1;
	}

	std::string out = absolute ? "/" : "";
	for (size_t i = 0; i < parts.size(); i++) {
		if (i)
			out += '/';
		out += parts[i];
	}
	return out;
}

bool ContentVFS::relativeToPrefix(const std::string &norm_path,
		const std::string &prefix, std::string *rel)
{
	if (norm_path == prefix) {
		rel->clear();
		return true;
	}
	if (norm_path.size() > prefix.size() + 1 &&
			norm_path.compare(0, prefix.size(), prefix) == 0 &&
			norm_path[prefix.size()] == '/') {
		*rel = norm_path.substr(prefix.size() + 1);
		return true;
	}
	return false;
}

std::string ContentVFS::resolveMountSpec(const std::string &spec)
{
	size_t sep = spec.find(":/");
	if (sep == std::string::npos)
		return "";
	const std::string scheme = spec.substr(0, sep);
	const std::string sub = spec.substr(sep + 2);

	std::string root;
	if (scheme == "share")
		root = porting::path_share;
	else if (scheme == "user")
		root = porting::path_user;
	else
		return "";

	std::string full = root;
	if (!sub.empty())
		full += "/" + sub;
	return normalizePath(full);
}

std::shared_ptr<const ContentVFS::MountTable> ContentVFS::table() const
{
	std::lock_guard<std::mutex> lock(m_table_mutex);
	return m_table;
}

bool ContentVFS::admitPack(const ContentPack &pack, Source source,
		std::string &prefix, std::string &err) const
{
	// World packs are templates to copy into a world, never mounts.
	if (pack.type() == "world") {
		err = WORLD_PACK_NOT_MOUNTABLE;
		return false;
	}
	// Code ships only with the application: nothing that could carry
	// bytecode is accepted from the installed (downloaded) directory.
	if (source == Source::Installed && pack.type() != "content") {
		err = "only 'content' packs may be mounted from the installed packs "
				"directory (pack type is '" + pack.type() + "')";
		return false;
	}

	// Bytecode is LuaJIT-version-locked (FORMAT.md §9): refusing the whole
	// mount here beats confusing chunk-load errors deep inside mod loading.
	if (!pack.luajitVersion().empty() &&
			pack.luajitVersion() != runtimeLuaJITVersion()) {
		const std::string runtime = runtimeLuaJITVersion();
		err = "pack bytecode was compiled by LuaJIT " + pack.luajitVersion() +
				" but the engine runs " +
				(runtime.empty() ? "without LuaJIT" : ("LuaJIT " + runtime));
		return false;
	}

	// A pack may demand a newer application than the one running: its
	// registration code is not there yet, so it must not be mounted
	// (CONTENT_DELIVERY_DESIGN §5.1). No app version = no gate.
	if (!m_app_version.empty() && !pack.minAppVersion().empty() &&
			compareVersions(pack.minAppVersion(), m_app_version) > 0) {
		err = "pack requires app version " + pack.minAppVersion() +
				" but " + m_app_version + " is running";
		return false;
	}

	prefix = resolveMountSpec(pack.mountSpec());
	if (prefix.empty()) {
		err = "unsupported mount spec '" + pack.mountSpec() + "'";
		return false;
	}
	return true;
}

bool ContentVFS::mountPackFile(const std::string &pack_path, Source source,
		std::string &err)
{
	std::string err_open;
	std::unique_ptr<ContentPack> opened =
			ContentPack::open(pack_path, m_key_provider, err_open);
	if (!opened) {
		err = err_open;
		return false;
	}
	std::shared_ptr<ContentPack> pack(std::move(opened));

	std::string prefix;
	if (!admitPack(*pack, source, prefix, err))
		return false;

	{
		std::lock_guard<std::mutex> lock(m_write_mutex);
		for (const Candidate &c : m_candidates) {
			if (c.pack->fileIdentity() == pack->fileIdentity() &&
					c.pack->filePath() == pack->filePath()) {
				err = "pack file is already mounted";
				return false;
			}
		}
		actionstream << "ContentVFS: mounted " << sourceName(source) << " pack "
				<< pack->id() << " v" << pack->version()
				<< " (" << pack->entries().size() << " files"
				<< (pack->isEncrypted() ? ", encrypted" : "") << ") at "
				<< prefix << std::endl;
		addCandidate(source, std::move(prefix), std::move(pack));
	}
	return true;
}

void ContentVFS::addCandidate(Source source, std::string prefix,
		std::shared_ptr<ContentPack> pack)
{
	// Same file path = the delivery layer replaced the file in place
	// (atomic rename): the old candidate goes, whatever its id.
	for (auto it = m_candidates.begin(); it != m_candidates.end(); ++it) {
		if (it->pack->filePath() == pack->filePath()) {
			m_candidates.erase(it);
			break;
		}
	}
	m_candidates.push_back({source, std::move(prefix), std::move(pack)});
	republish();
}

void ContentVFS::republish()
{
	// Winner per id: the highest version; on equal versions the installed
	// copy; id as a deterministic tie-break.
	std::vector<const Candidate *> ordered;
	ordered.reserve(m_candidates.size());
	for (const Candidate &c : m_candidates)
		ordered.push_back(&c);
	std::stable_sort(ordered.begin(), ordered.end(),
			[](const Candidate *a, const Candidate *b) {
		if (a->pack->version() != b->pack->version())
			return a->pack->version() > b->pack->version();
		if (a->source != b->source)
			return a->source == Source::Installed;
		return a->pack->id() < b->pack->id();
	});

	// One visible pack per id: the highest version; on equal versions the
	// installed copy (a shadowed bundled twin is the swap-window case).
	auto fresh = std::make_shared<MountTable>();
	std::set<std::string> ids;
	for (const Candidate *c : ordered) {
		if (!ids.insert(c->pack->id()).second) {
			// Expected during a delivery swap and for a bundled twin of an
			// installed pack; repeated on every republish, so keep it quiet.
			infostream << "ContentVFS: " << sourceName(c->source) << " pack "
					<< c->pack->id() << " v" << c->pack->version()
					<< " is shadowed by a newer copy" << std::endl;
			continue;
		}
		fresh->push_back({c->source, c->prefix, c->pack});
	}
	// Lookup order among the winners: installed first, then higher version,
	// then id.
	std::stable_sort(fresh->begin(), fresh->end(),
			[](const MountInfo &a, const MountInfo &b) {
		if (a.source != b.source)
			return a.source == Source::Installed;
		if (a.pack->version() != b.pack->version())
			return a.pack->version() > b.pack->version();
		return a.pack->id() < b.pack->id();
	});

	// A path provided by two different packs is a packaging bug the
	// catalog builder is supposed to catch; here it only gets a warning
	// so a stray one is visible instead of silently resolved.
	for (size_t i = 0; i < fresh->size(); i++) {
		for (size_t j = i + 1; j < fresh->size(); j++) {
			const MountInfo &hi = (*fresh)[i];
			const MountInfo &lo = (*fresh)[j];
			if (hi.prefix != lo.prefix)
				continue;
			size_t collisions = 0;
			std::string example;
			const auto &small = hi.pack->entries().size() <= lo.pack->entries().size()
					? hi.pack->entries() : lo.pack->entries();
			const ContentPack &other = hi.pack->entries().size() <= lo.pack->entries().size()
					? *lo.pack : *hi.pack;
			for (const auto &e : small) {
				if (other.findEntry(e.first)) {
					if (collisions++ == 0)
						example = e.first;
				}
			}
			if (collisions) {
				warningstream << "ContentVFS: packs " << hi.pack->id() << " and "
						<< lo.pack->id() << " both provide " << collisions
						<< " path(s) under " << hi.prefix << " (e.g. " << example
						<< "); " << hi.pack->id() << " wins" << std::endl;
			}
		}
	}

	{
		std::lock_guard<std::mutex> lock(m_table_mutex);
		m_table = fresh;
	}
	m_active.store(!fresh->empty(), std::memory_order_release);

#if CHECK_CLIENT_BUILD()
	if (!fresh->empty() && !m_fetcher_installed) {
		m_fetcher_installed = true;
		io::setExternalFileFetcher(contentVFSIrrFetcher);
	}
#endif
}

void ContentVFS::mountPacksFromDir(const std::string &dir, Source source)
{
	// Pack files are always real files; no mount covers a packs directory,
	// so the overlay adds nothing to this listing.
	for (const fs::DirListNode &node : fs::GetDirListing(dir)) {
		if (node.dir || node.name.size() < 5 ||
				node.name.compare(node.name.size() - 4, 4, ".kpk") != 0)
			continue;
		const std::string path = dir + DIR_DELIM + node.name;
		std::string err;
		if (mountPackFile(path, source, err))
			continue;
		if (err == WORLD_PACK_NOT_MOUNTABLE)
			verbosestream << "ContentVFS: skipping world pack " << path << std::endl;
		else
			errorstream << "ContentVFS: failed to mount " << path
					<< ": " << err << std::endl;
	}
}

void ContentVFS::mountStartupPacks()
{
	mountPacksFromDir(porting::path_share + DIR_DELIM + "packs", Source::Bundled);
	syncInstalledPacks();
}

void ContentVFS::logMountSummary() const
{
	const std::vector<MountInfo> mounts = getMounts();
	actionstream << "ContentVFS: " << mounts.size() << " pack(s) mounted; installed packs directory: "
			<< installedPacksDir() << std::endl;
	for (const MountInfo &m : mounts) {
		actionstream << "ContentVFS:   " << sourceName(m.source) << " " << m.pack->id()
				<< " v" << m.pack->version()
				<< " (" << m.pack->entries().size() << " files"
				<< (m.pack->isEncrypted() ? ", encrypted" : "") << ") at "
				<< m.prefix << std::endl;
	}
}

void ContentVFS::syncInstalledPacks()
{
	const std::string dir = installedPacksDir();

	// Run-in-place builds have path_user == path_share: the "installed"
	// directory is then the bundled one, already mounted as such — do not
	// reconcile the same files a second time under installed rules.
	if (dir == normalizePath(porting::path_share + "/packs"))
		return;

	// What is on disk now: regular *.kpk files with their identity.
	std::map<std::string, ContentPack::FileIdentity> on_disk;
	for (const fs::DirListNode &node : fs::GetDirListing(dir)) {
		if (node.dir || node.name.size() < 5 ||
				node.name.compare(node.name.size() - 4, 4, ".kpk") != 0)
			continue;
		const std::string path = normalizePath(dir + "/" + node.name);
		ContentPack::FileIdentity id;
		if (ContentPack::statIdentity(path, id))
			on_disk[path] = id;
	}

	std::vector<std::string> to_open;
	{
		std::lock_guard<std::mutex> lock(m_write_mutex);

		// Drop installed candidates whose file vanished or changed identity;
		// changed ones are reopened below (their new inode may be a
		// different pack version — or garbage, which then just fails).
		std::set<std::string> still_current;
		bool changed = false;
		const std::string dir_prefix = dir + "/";
		for (auto it = m_candidates.begin(); it != m_candidates.end();) {
			// Only packs that live in the installed directory are reconciled
			// against it (a test or tool may mount an installed-class pack
			// from elsewhere; that one is left alone).
			if (it->source != Source::Installed ||
					it->pack->filePath().compare(0, dir_prefix.size(), dir_prefix) != 0) {
				++it;
				continue;
			}
			const auto now = on_disk.find(it->pack->filePath());
			if (now != on_disk.end() && now->second == it->pack->fileIdentity()) {
				still_current.insert(it->pack->filePath());
				++it;
				continue;
			}
			actionstream << "ContentVFS: installed pack " << it->pack->id()
					<< " v" << it->pack->version()
					<< (now != on_disk.end() ? " changed on disk" : " was removed")
					<< std::endl;
			it = m_candidates.erase(it);
			changed = true;
		}
		if (changed)
			republish();

		// Files refused earlier are not retried (or re-logged) until they
		// change; entries for vanished files are forgotten.
		for (auto it = m_ignored_installed.begin(); it != m_ignored_installed.end();) {
			const auto now = on_disk.find(it->first);
			if (now == on_disk.end() || now->second != it->second)
				it = m_ignored_installed.erase(it);
			else
				++it;
		}

		for (const auto &kv : on_disk) {
			if (!still_current.count(kv.first) && !m_ignored_installed.count(kv.first))
				to_open.push_back(kv.first);
		}
	}

	for (const std::string &path : to_open) {
		std::string err;
		if (mountPackFile(path, Source::Installed, err))
			continue;
		if (err == WORLD_PACK_NOT_MOUNTABLE)
			verbosestream << "ContentVFS: not mounting world pack " << path << std::endl;
		else
			errorstream << "ContentVFS: failed to mount installed pack " << path
					<< ": " << err << std::endl;
		std::lock_guard<std::mutex> lock(m_write_mutex);
		m_ignored_installed[path] = on_disk[path];
	}
}

std::string ContentVFS::runtimeLuaJITVersion()
{
#if USE_LUAJIT
	std::string v(LUAJIT_VERSION); // "LuaJIT 2.1.1785577137"
	const std::string prefix = "LuaJIT ";
	if (v.compare(0, prefix.size(), prefix) == 0)
		return v.substr(prefix.size());
	return v;
#else
	return "";
#endif
}

bool ContentVFS::isTrustedCodePath(const std::string &path) const
{
	if (!isActive())
		return false;

	// A loose file shadows the pack entry and is what fs::ReadFile actually
	// served — it must never inherit pack-level trust (someone could plant
	// bytecode at a path that also exists inside a pack).
	if (fs::PathExistsNative(path))
		return false;

	const std::string norm = normalizePath(path);
	std::string rel;
	const auto t = table();
	for (const MountInfo &m : *t) {
		if (!relativeToPrefix(norm, m.prefix, &rel) || rel.empty())
			continue;
		if (m.pack->findEntry(rel))
			return m.pack->type() == "base" && m.source == Source::Bundled;
	}
	return false;
}

void ContentVFS::unmountAll()
{
	std::lock_guard<std::mutex> lock(m_write_mutex);
	m_candidates.clear();
	m_ignored_installed.clear();
	republish();
}

std::shared_ptr<const ContentPack> ContentVFS::getPack(const std::string &id) const
{
	const auto t = table();
	for (const MountInfo &m : *t) {
		if (m.pack->id() == id)
			return m.pack;
	}
	return nullptr;
}

std::vector<ContentVFS::MountInfo> ContentVFS::getMounts() const
{
	return *table();
}

ContentVFS::Stat ContentVFS::statPath(const std::string &path) const
{
	if (!isActive())
		return Stat::NotFound;

	const std::string norm = normalizePath(path);
	std::string rel;
	const auto t = table();
	for (const MountInfo &m : *t) {
		if (relativeToPrefix(norm, m.prefix, &rel)) {
			if (rel.empty())
				return Stat::Dir; // the mount point itself
			if (m.pack->findEntry(rel))
				return Stat::File;
			// directory iff some entry lives below rel/
			auto it = m.pack->entries().lower_bound(rel + "/");
			if (it != m.pack->entries().end() &&
					it->first.compare(0, rel.size() + 1, rel + "/") == 0)
				return Stat::Dir;
		} else {
			// an ancestor of the mount prefix is a directory
			std::string tmp;
			if (relativeToPrefix(m.prefix, norm, &tmp))
				return Stat::Dir;
		}
	}
	return Stat::NotFound;
}

bool ContentVFS::readFile(const std::string &path, std::string &out) const
{
	if (!isActive())
		return false;

	const std::string norm = normalizePath(path);
	std::string rel;
	const auto t = table();
	for (const MountInfo &m : *t) {
		if (!relativeToPrefix(norm, m.prefix, &rel) || rel.empty())
			continue;
		const ContentPack::Entry *e = m.pack->findEntry(rel);
		if (e)
			return m.pack->readEntry(*e, out);
	}
	return false;
}

void ContentVFS::addDirEntries(const std::string &path,
		std::vector<fs::DirListNode> &listing) const
{
	if (!isActive())
		return;

	const std::string norm = normalizePath(path);

	std::set<std::string> seen;
	for (const fs::DirListNode &n : listing)
		seen.insert(n.name);

	std::string rel;
	const auto t = table();
	for (const MountInfo &m : *t) {
		if (relativeToPrefix(norm, m.prefix, &rel)) {
			// children of `rel` inside the pack
			const std::string want = rel.empty() ? "" : rel + "/";
			auto it = want.empty() ? m.pack->entries().begin()
					: m.pack->entries().lower_bound(want);
			for (; it != m.pack->entries().end(); ++it) {
				if (!want.empty() &&
						it->first.compare(0, want.size(), want) != 0)
					break;
				const std::string remainder = it->first.substr(want.size());
				const size_t slash = remainder.find('/');
				std::string child = remainder.substr(0, slash);
				if (child.empty() || !seen.insert(child).second)
					continue;
				fs::DirListNode node;
				node.name = std::move(child);
				node.dir = slash != std::string::npos;
				listing.push_back(std::move(node));
			}
		} else {
			// `path` is an ancestor of the mount prefix: surface the next
			// component of the prefix as a directory
			std::string below;
			if (relativeToPrefix(m.prefix, norm, &below) && !below.empty()) {
				std::string child = below.substr(0, below.find('/'));
				if (!child.empty() && seen.insert(child).second) {
					fs::DirListNode node;
					node.name = std::move(child);
					node.dir = true;
					listing.push_back(std::move(node));
				}
			}
		}
	}
}

std::string ContentVFS::normalizedIfCovered(const std::string &path) const
{
	if (!isActive())
		return "";
	const std::string norm = normalizePath(path);
	return statPath(norm) != Stat::NotFound ? norm : "";
}
