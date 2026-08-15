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

#ifndef _WIN32
	#include <fcntl.h>
	#include <sys/stat.h>
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

} // namespace

// Weak default: no key provider. Overridden by the platform layer's strong
// definition when it is linked in (MSVC has no weak symbols; there the
// platform layer must call ContentVFS::setKeyProvider() itself before mount).
#if defined(__GNUC__) || defined(__clang__)
extern "C" __attribute__((weak)) void vopi_install_content_key_provider()
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

/*
	ContentVFS
*/

ContentVFS &ContentVFS::get()
{
	static ContentVFS instance;
	return instance;
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

bool ContentVFS::mountPackFile(const std::string &pack_path, std::string &err)
{
	auto pack = ContentPack::open(pack_path, m_key_provider, err);
	if (!pack)
		return false;

	if (getPack(pack->id())) {
		err = "pack id '" + pack->id() + "' is already mounted";
		return false;
	}

	// Bytecode is LuaJIT-version-locked (FORMAT.md §9): refusing the whole
	// mount here beats confusing chunk-load errors deep inside mod loading.
	if (!pack->luajitVersion().empty() &&
			pack->luajitVersion() != runtimeLuaJITVersion()) {
		const std::string runtime = runtimeLuaJITVersion();
		err = "pack bytecode was compiled by LuaJIT " + pack->luajitVersion() +
				" but the engine runs " +
				(runtime.empty() ? "without LuaJIT" : ("LuaJIT " + runtime));
		return false;
	}

	std::string prefix = resolveMountSpec(pack->mountSpec());
	if (prefix.empty()) {
		err = "unsupported mount spec '" + pack->mountSpec() + "'";
		return false;
	}

	actionstream << "ContentVFS: mounted " << pack->id()
			<< " v" << pack->version()
			<< " (" << pack->entries().size() << " files"
			<< (pack->isEncrypted() ? ", encrypted" : "") << ") at "
			<< prefix << std::endl;

	m_mounts.push_back({std::move(prefix), std::move(pack)});
	if (!m_active) {
		m_active = true;
#if CHECK_CLIENT_BUILD()
		io::setExternalFileFetcher(contentVFSIrrFetcher);
#endif
	}
	return true;
}

void ContentVFS::mountPacksFromDir(const std::string &dir)
{
	// Native listing on purpose: pack files are always real files.
	for (const fs::DirListNode &node : fs::GetDirListing(dir)) {
		if (node.dir || node.name.size() < 5 ||
				node.name.compare(node.name.size() - 4, 4, ".kpk") != 0)
			continue;
		const std::string path = dir + DIR_DELIM + node.name;
		std::string err;
		if (!mountPackFile(path, err))
			errorstream << "ContentVFS: failed to mount " << path
					<< ": " << err << std::endl;
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
	if (!m_active)
		return false;

	// A loose file shadows the pack entry and is what fs::ReadFile actually
	// served — it must never inherit pack-level trust (someone could plant
	// bytecode at a path that also exists inside a pack).
	if (fs::PathExistsNative(path))
		return false;

	const std::string norm = normalizePath(path);
	std::string rel;
	for (const Mount &m : m_mounts) {
		if (!relativeToPrefix(norm, m.prefix, &rel) || rel.empty())
			continue;
		if (m.pack->findEntry(rel))
			return m.pack->type() == "base";
	}
	return false;
}

void ContentVFS::unmountAll()
{
	m_mounts.clear();
	m_active = false;
}

const ContentPack *ContentVFS::getPack(const std::string &id) const
{
	for (const Mount &m : m_mounts) {
		if (m.pack->id() == id)
			return m.pack.get();
	}
	return nullptr;
}

ContentVFS::Stat ContentVFS::statPath(const std::string &path) const
{
	if (!m_active)
		return Stat::NotFound;

	const std::string norm = normalizePath(path);
	std::string rel;
	for (const Mount &m : m_mounts) {
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
	if (!m_active)
		return false;

	const std::string norm = normalizePath(path);
	std::string rel;
	for (const Mount &m : m_mounts) {
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
	if (!m_active)
		return;

	const std::string norm = normalizePath(path);

	std::set<std::string> seen;
	for (const fs::DirListNode &n : listing)
		seen.insert(n.name);

	std::string rel;
	for (const Mount &m : m_mounts) {
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
	if (!m_active)
		return "";
	const std::string norm = normalizePath(path);
	return statPath(norm) != Stat::NotFound ? norm : "";
}
