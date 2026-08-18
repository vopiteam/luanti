// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#include "test.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "aes_ctr.h"
#include "config.h"
#include "content_vfs.h"
#include "filesys.h"
#include "log.h"
#include "porting.h"
#if CHECK_CLIENT_BUILD()
#include "client/clientmedia.h"
#include "util/hashing.h"
#include "util/hex.h"
#endif

// Builds minimal KPK v1 containers in-process (tools/kpk/FORMAT.md) and
// exercises the ContentVFS overlay through the public fs:: API — the same
// route the engine takes for builtin, game discovery and media.
class TestContentVFS : public TestBase
{
public:
	TestContentVFS() { TestManager::registerTestModule(this); }
	const char *getName() { return "TestContentVFS"; }

	void runTests(IGameDef *gamedef);

	void testRejectsGarbage();
	void testMountAndStat();
	void testReadThroughFs();
	void testDirListingUnion();
	void testRecursiveDirs();
	void testRealFileWins();
	void testBytecodeTrust();
	void testEncryptedPack();
	void testMediaCacheSkipsPackFiles();
	void testCompareVersions();
	void testMinAppVersion();
	void testInstalledPacksSync();
	void testPriority();

private:
	struct PackOpts {
		std::string mount_spec;
		std::string id = "vfstest";
		std::string type = "base";
		std::string luajit_version;
		int first_entry_flags = 0;
		const unsigned char *aes_key = nullptr;
		u32 key_version = 0;
		int version = 1;
		std::string min_app_version;
		std::string a_content = "hello from pack";
		std::string out_dir;   // default: test temp dir
		std::string file_name; // default: <id>.kpk
	};
	std::string makePack(const PackOpts &o);

	std::string m_pack_path;
	std::string m_mount_prefix; // resolved absolute prefix
	std::string m_installed_dir;
};

static TestContentVFS g_test_instance;

static void putLEU32(std::string &s, u32 v)
{
	for (int i = 0; i < 4; i++)
		s += (char)((v >> (8 * i)) & 0xff);
}

static void putLEU64(std::string &s, u64 v)
{
	for (int i = 0; i < 8; i++)
		s += (char)((v >> (8 * i)) & 0xff);
}

static const char *hexdigits = "0123456789abcdef";
static std::string toHex(const unsigned char *b, size_t n)
{
	std::string s;
	for (size_t i = 0; i < n; i++) {
		s += hexdigits[b[i] >> 4];
		s += hexdigits[b[i] & 15];
	}
	return s;
}

// Three entries, stored raw, uncompressed index (INDEX_ZSTD off).
// With aes_key: entries and index are AES-256-CTR encrypted with fixed
// per-object nonces (FORMAT.md §6/§7 layout; nonce derivation is the
// packer's business — the reader only uses the stored values).
std::string TestContentVFS::makePack(const PackOpts &o)
{
	std::string a_content = o.a_content;
	std::string b_content = std::string("\x01\x02\x03", 3);

	unsigned char nonce_a[16], nonce_b[16], nonce_index[16];
	for (int i = 0; i < 16; i++) {
		nonce_a[i] = (unsigned char)(0xA0 + i);
		nonce_b[i] = (unsigned char)(0xB0 + i);
		nonce_index[i] = (unsigned char)(0xC0 + i);
	}
	aes256_key kctx;
	if (o.aes_key) {
		aes256_set_key(&kctx, o.aes_key);
		aes256_ctr_xor(&kctx, nonce_a, (const uint8_t *)a_content.data(),
				(uint8_t *)&a_content[0], a_content.size());
		aes256_ctr_xor(&kctx, nonce_b, (const uint8_t *)b_content.data(),
				(uint8_t *)&b_content[0], b_content.size());
	}
	const std::string n_a = o.aes_key ? toHex(nonce_a, 16) : std::string(32, '0');
	const std::string n_b = o.aes_key ? toHex(nonce_b, 16) : std::string(32, '0');

	std::string meta = "{\"id\":\"" + o.id + "\",\"mount\":\"" + o.mount_spec +
			"\",\"type\":\"" + o.type + "\",\"version\":" + std::to_string(o.version);
	if (!o.luajit_version.empty())
		meta += ",\"luajitVersion\":\"" + o.luajit_version + "\"";
	if (!o.min_app_version.empty())
		meta += ",\"minAppVersion\":\"" + o.min_app_version + "\"";
	meta += "}";

	const u64 meta_offset = 80;
	const u64 blob_start = meta_offset + meta.size();

	// entries in index (sorted by path): a.txt, e.txt (empty), sub/b.bin
	std::ostringstream index_ss;
	index_ss << "{\"files\":["
		<< "{\"p\":\"a.txt\",\"o\":" << blob_start
		<< ",\"s\":" << a_content.size() << ",\"r\":" << a_content.size()
		<< ",\"h\":\"0000000000000000000000000000000000000000\",\"n\":\""
		<< n_a << "\",\"f\":" << o.first_entry_flags << "},"
		<< "{\"p\":\"e.txt\",\"o\":" << (blob_start + a_content.size())
		<< ",\"s\":0,\"r\":0"
		<< ",\"h\":\"0000000000000000000000000000000000000000\",\"n\":\""
		<< std::string(32, '0') << "\",\"f\":0},"
		<< "{\"p\":\"sub/b.bin\",\"o\":" << (blob_start + a_content.size())
		<< ",\"s\":" << b_content.size() << ",\"r\":" << b_content.size()
		<< ",\"h\":\"0000000000000000000000000000000000000000\",\"n\":\""
		<< n_b << "\",\"f\":0}"
		<< "]}";
	std::string index = index_ss.str();
	const u64 index_offset = blob_start + a_content.size() + b_content.size();
	if (o.aes_key) {
		aes256_ctr_xor(&kctx, nonce_index, (const uint8_t *)index.data(),
				(uint8_t *)&index[0], index.size());
	}

	std::string blob;
	blob += "KPK1";
	putLEU32(blob, 1);  // format version
	putLEU32(blob, o.aes_key ? 1 : 0);  // flags: ENCRYPTED bit; index not compressed
	putLEU32(blob, o.aes_key ? o.key_version : 0);  // key version
	putLEU64(blob, meta_offset);
	putLEU64(blob, meta.size());
	putLEU64(blob, 0);  // preview offset
	putLEU64(blob, 0);  // preview size
	putLEU64(blob, index_offset);
	putLEU64(blob, index.size());
	blob += o.aes_key ? std::string((const char *)nonce_index, 16)
			: std::string(16, '\0'); // index nonce
	blob += meta;
	blob += a_content;
	blob += b_content;
	blob += index;

	const std::string dir = o.out_dir.empty() ? getTestTempDirectory() : o.out_dir;
	const std::string name = o.file_name.empty() ? o.id + ".kpk" : o.file_name;
	const std::string path = dir + DIR_DELIM + name;
	// Write to a temp name and rename over, like the delivery layer does;
	// also guarantees the mtime moves on when a test rewrites a file.
	const std::string tmp = path + ".tmp";
	std::ofstream os(tmp, std::ios::binary);
	os << blob;
	os.close();
	fs::DeleteSingleFileOrEmptyDirectory(path);
	fs::Rename(tmp, path);
	return path;
}

void TestContentVFS::runTests(IGameDef *gamedef)
{
	// Unique virtual prefix under path_user; nothing real exists there.
	const std::string mount_spec = "user:/__vfs_selftest__";
	m_mount_prefix = porting::path_user + DIR_DELIM + "__vfs_selftest__";
	PackOpts o;
	o.mount_spec = mount_spec;
	m_pack_path = makePack(o);

	m_installed_dir = getTestTempDirectory() + DIR_DELIM + "installed_packs";
	fs::RecursiveDelete(m_installed_dir);
	fs::CreateAllDirs(m_installed_dir);
	ContentVFS::get().setInstalledPacksDir(m_installed_dir);

	TEST(testRejectsGarbage);
	TEST(testMountAndStat);
	TEST(testReadThroughFs);
	TEST(testDirListingUnion);
	TEST(testRecursiveDirs);
	TEST(testRealFileWins);
	TEST(testBytecodeTrust);
	TEST(testEncryptedPack);
	TEST(testMediaCacheSkipsPackFiles);
	TEST(testCompareVersions);
	TEST(testMinAppVersion);
	TEST(testInstalledPacksSync);
	TEST(testPriority);

	ContentVFS::get().unmountAll();
	ContentVFS::get().setKeyProvider(nullptr);
	ContentVFS::get().setAppVersion("");
	ContentVFS::get().setInstalledPacksDir("");
	fs::RecursiveDelete(m_mount_prefix);
	fs::RecursiveDelete(m_installed_dir);
}

// Test key provider: knows key_version 7 only.
static const unsigned char TEST_AES_KEY[32] = {
	0x60,0x3d,0xeb,0x10,0x15,0xca,0x71,0xbe,0x2b,0x73,0xae,0xf0,0x85,0x7d,0x77,0x81,
	0x1f,0x35,0x2c,0x07,0x3b,0x61,0x08,0xd7,0x2d,0x98,0x10,0xa3,0x09,0x14,0xdf,0xf4
};
static bool testKeyProvider(u32 key_version, unsigned char key_out[32])
{
	if (key_version != 7)
		return false;
	memcpy(key_out, TEST_AES_KEY, 32);
	return true;
}

void TestContentVFS::testRejectsGarbage()
{
	const std::string bad_path = getTestTempDirectory() + DIR_DELIM + "bad.kpk";
	std::ofstream os(bad_path, std::ios::binary);
	os << "definitely not a kpk file";
	os.close();

	std::string err;
	UASSERT(!ContentVFS::get().mountPackFile(bad_path, ContentVFS::Source::Bundled, err));
	UASSERT(!err.empty());

	// Crafted header whose meta block bounds wrap u64: offset + size
	// overflows past file_size and must still be rejected.
	std::string evil;
	evil += "KPK1";
	putLEU32(evil, 1); // format version
	putLEU32(evil, 0); // flags
	putLEU32(evil, 0); // key version
	putLEU64(evil, 0xFFFFFFFFFFFFFFF0ULL); // meta_offset (wraps with size)
	putLEU64(evil, 0x20);                  // meta_size
	putLEU64(evil, 0); // preview offset
	putLEU64(evil, 0); // preview size
	putLEU64(evil, 80); // index offset
	putLEU64(evil, 8);  // index size
	evil += std::string(16, '\0');
	evil += std::string(64, 'x'); // trailing bytes so blocks "fit"

	const std::string evil_path = getTestTempDirectory() + DIR_DELIM + "evil.kpk";
	std::ofstream eos(evil_path, std::ios::binary);
	eos << evil;
	eos.close();

	UASSERT(!ContentVFS::get().mountPackFile(evil_path, ContentVFS::Source::Bundled, err));
}

void TestContentVFS::testMountAndStat()
{
	std::string err;
	bool mounted = ContentVFS::get().mountPackFile(m_pack_path,
			ContentVFS::Source::Bundled, err);
	if (!mounted)
		errorstream << "mountPackFile: " << err << std::endl;
	UASSERT(mounted);
	UASSERT(ContentVFS::get().isActive());
	UASSERT(ContentVFS::get().getPack("vfstest") != nullptr);

	// the very same file is refused a second time
	UASSERT(!ContentVFS::get().mountPackFile(m_pack_path, ContentVFS::Source::Bundled, err));

	UASSERT(ContentVFS::get().statPath(m_mount_prefix) == ContentVFS::Stat::Dir);
	UASSERT(ContentVFS::get().statPath(m_mount_prefix + DIR_DELIM + "a.txt")
			== ContentVFS::Stat::File);
	UASSERT(ContentVFS::get().statPath(m_mount_prefix + DIR_DELIM + "sub")
			== ContentVFS::Stat::Dir);
	UASSERT(ContentVFS::get().statPath(m_mount_prefix + DIR_DELIM + "nope")
			== ContentVFS::Stat::NotFound);
}

void TestContentVFS::testReadThroughFs()
{
	// The whole point of the overlay: plain fs:: calls see pack content.
	UASSERT(fs::PathExists(m_mount_prefix + DIR_DELIM + "a.txt"));
	UASSERT(fs::IsFile(m_mount_prefix + DIR_DELIM + "a.txt"));
	UASSERT(fs::IsDir(m_mount_prefix + DIR_DELIM + "sub"));
	UASSERT(!fs::IsFile(m_mount_prefix + DIR_DELIM + "sub"));

	std::string content;
	UASSERT(fs::ReadFile(m_mount_prefix + DIR_DELIM + "a.txt", content, false));
	UASSERTEQ(std::string, content, "hello from pack");

	UASSERT(fs::ReadFile(m_mount_prefix + DIR_DELIM + "sub" + DIR_DELIM + "b.bin",
			content, false));
	UASSERTEQ(size_t, content.size(), 3);

	UASSERT(fs::ReadFile(m_mount_prefix + DIR_DELIM + "e.txt", content, false));
	UASSERT(content.empty());

	// AbsolutePath falls back to the lexical form for covered paths
	const std::string abs = fs::AbsolutePath(
			m_mount_prefix + DIR_DELIM + "sub" + DIR_DELIM + ".." + DIR_DELIM + "a.txt");
	UASSERT(!abs.empty());
	UASSERT(abs.find("..") == std::string::npos);
}

void TestContentVFS::testDirListingUnion()
{
	// Mount root listing comes purely from the pack
	std::vector<fs::DirListNode> listing = fs::GetDirListing(m_mount_prefix);
	bool saw_a = false, saw_sub = false, saw_e = false;
	for (const auto &n : listing) {
		if (n.name == "a.txt") { saw_a = true; UASSERT(!n.dir); }
		if (n.name == "sub")   { saw_sub = true; UASSERT(n.dir); }
		if (n.name == "e.txt") { saw_e = true; UASSERT(!n.dir); }
	}
	UASSERT(saw_a && saw_sub && saw_e);

	// The mount point surfaces as a directory when listing its real parent
	bool saw_mount = false;
	for (const auto &n : fs::GetDirListing(porting::path_user)) {
		if (n.name == "__vfs_selftest__") {
			saw_mount = true;
			UASSERT(n.dir);
		}
	}
	UASSERT(saw_mount);
}

void TestContentVFS::testRecursiveDirs()
{
	// The server collects mod media with fs::GetRecursiveDirs (textures/,
	// models/, ... and every subdirectory): nested media directories inside
	// a pack must surface exactly like loose ones — this is what makes the
	// textures/extra/<set>/ layout of downloadable content work.
	std::vector<std::string> dirs;
	fs::GetRecursiveDirs(dirs, m_mount_prefix);
	bool saw_root = false, saw_sub = false;
	for (const std::string &d : dirs) {
		if (d == m_mount_prefix)
			saw_root = true;
		if (d == m_mount_prefix + DIR_DELIM + "sub")
			saw_sub = true;
	}
	UASSERT(saw_root);
	UASSERT(saw_sub);

	// ...and the files inside the nested directory are listed
	bool saw_b = false;
	for (const auto &n : fs::GetDirListing(m_mount_prefix + DIR_DELIM + "sub")) {
		if (n.name == "b.bin") {
			saw_b = true;
			UASSERT(!n.dir);
		}
	}
	UASSERT(saw_b);
}

void TestContentVFS::testRealFileWins()
{
	// A loose file at the same path must shadow the pack entry (dev overlay)
	const std::string dir = m_mount_prefix;
	const std::string real_file = dir + DIR_DELIM + "a.txt";
	UASSERT(fs::CreateAllDirs(dir));
	std::ofstream os(real_file, std::ios::binary);
	os << "real file wins";
	os.close();

	std::string content;
	UASSERT(fs::ReadFile(real_file, content, false));
	UASSERTEQ(std::string, content, "real file wins");

	fs::DeleteSingleFileOrEmptyDirectory(real_file);
	UASSERT(fs::ReadFile(real_file, content, false));
	UASSERTEQ(std::string, content, "hello from pack");
}

void TestContentVFS::testBytecodeTrust()
{
	std::string err;

	// A pack whose bytecode was produced by a different LuaJIT must be
	// refused at mount time.
	PackOpts stale;
	stale.mount_spec = "user:/__vfs_stale__";
	stale.id = "vfsstale";
	stale.luajit_version = "0.0.bogus";
	UASSERT(!ContentVFS::get().mountPackFile(makePack(stale),
			ContentVFS::Source::Bundled, err));
	UASSERT(err.find("LuaJIT") != std::string::npos);

	// Bytecode entries without a recorded luajitVersion cannot be
	// version-gated — the mount must be refused too.
	PackOpts nolj;
	nolj.mount_spec = "user:/__vfs_nolj__";
	nolj.id = "vfsnolj";
	nolj.first_entry_flags = ContentPack::EFLAG_LUA_BYTECODE;
	UASSERT(!ContentVFS::get().mountPackFile(makePack(nolj),
			ContentVFS::Source::Bundled, err));
	UASSERT(err.find("luajitVersion") != std::string::npos);

	// Entries of the mounted "base" pack are trusted code paths...
	const std::string trusted_file = m_mount_prefix + DIR_DELIM + "a.txt";
	UASSERT(ContentVFS::get().isTrustedCodePath(trusted_file));
	// ...but not directories, absent entries or uncovered paths
	UASSERT(!ContentVFS::get().isTrustedCodePath(m_mount_prefix + DIR_DELIM + "sub"));
	UASSERT(!ContentVFS::get().isTrustedCodePath(m_mount_prefix + DIR_DELIM + "nope"));
	UASSERT(!ContentVFS::get().isTrustedCodePath(porting::path_user));

	// A downloadable ("content") pack never grants code trust
	PackOpts data;
	data.mount_spec = "user:/__vfs_data__";
	data.id = "vfsdata";
	data.type = "content";
	UASSERT(ContentVFS::get().mountPackFile(makePack(data),
			ContentVFS::Source::Bundled, err));
	UASSERT(!ContentVFS::get().isTrustedCodePath(
			porting::path_user + DIR_DELIM + "__vfs_data__" + DIR_DELIM + "a.txt"));

	// A loose file shadowing the pack entry strips the trust: the shadowing
	// file is what fs::ReadFile actually serves.
	UASSERT(fs::CreateAllDirs(m_mount_prefix));
	std::ofstream os(trusted_file, std::ios::binary);
	os << "planted";
	os.close();
	UASSERT(!ContentVFS::get().isTrustedCodePath(trusted_file));
	fs::DeleteSingleFileOrEmptyDirectory(trusted_file);
	UASSERT(ContentVFS::get().isTrustedCodePath(trusted_file));
}

void TestContentVFS::testEncryptedPack()
{
	std::string err;
	const std::string enc_prefix = porting::path_user + DIR_DELIM + "__vfs_enc__";
	PackOpts enc;
	enc.mount_spec = "user:/__vfs_enc__";
	enc.id = "vfsenc";
	enc.aes_key = TEST_AES_KEY;
	enc.key_version = 7;
	const std::string enc_path = makePack(enc);

	// No provider installed: encrypted packs are refused outright
	ContentVFS::get().setKeyProvider(nullptr);
	UASSERT(!ContentVFS::get().mountPackFile(enc_path, ContentVFS::Source::Bundled, err));
	UASSERT(err.find("key provider") != std::string::npos);

	// Provider that does not know the pack's key_version
	ContentVFS::get().setKeyProvider(testKeyProvider);
	PackOpts enc9 = enc;
	enc9.mount_spec = "user:/__vfs_enc9__";
	enc9.id = "vfsenc9";
	enc9.key_version = 9;
	UASSERT(!ContentVFS::get().mountPackFile(makePack(enc9),
			ContentVFS::Source::Bundled, err));
	UASSERT(err.find("key_version 9") != std::string::npos);

	// Right key: mounts, and content decrypts through the plain fs:: API
	UASSERT(ContentVFS::get().mountPackFile(enc_path, ContentVFS::Source::Bundled, err));
	const auto pack = ContentVFS::get().getPack("vfsenc");
	UASSERT(pack && pack->isEncrypted() && pack->keyVersion() == 7);

	std::string content;
	UASSERT(fs::ReadFile(enc_prefix + DIR_DELIM + "a.txt", content, false));
	UASSERTEQ(std::string, content, "hello from pack");
	UASSERT(fs::ReadFile(enc_prefix + DIR_DELIM + "sub" + DIR_DELIM + "b.bin",
			content, false));
	UASSERTEQ(std::string, content, std::string("\x01\x02\x03", 3));
	UASSERT(fs::ReadFile(enc_prefix + DIR_DELIM + "e.txt", content, false));
	UASSERT(content.empty());
}

void TestContentVFS::testMediaCacheSkipsPackFiles()
{
#if CHECK_CLIENT_BUILD()
	// The local-server media path copies files into path_cache/media with a
	// raw fopen. Pack entries have no loose file: the copy must be declined
	// (not attempted, not logged as an error, no empty cache file left) —
	// the loopback transfer serves them. Loose files still get copied.
	const std::string cache_dir = porting::path_cache + DIR_DELIM + "media";

	// pack-served entry (mounted in testMountAndStat)
	const std::string pack_file = m_mount_prefix + DIR_DELIM + "a.txt";
	std::string pack_data;
	UASSERT(fs::ReadFile(pack_file, pack_data, false));
	const std::string pack_hash = hashing::sha1(pack_data);
	fs::DeleteSingleFileOrEmptyDirectory(cache_dir + DIR_DELIM + hex_encode(pack_hash));

	UASSERT(!clientMediaUpdateCacheCopy(pack_hash, pack_file));
	UASSERT(!fs::PathExistsNative(cache_dir + DIR_DELIM + hex_encode(pack_hash)));

	// loose file: copied as before
	const std::string loose_file = getTestTempDirectory() + DIR_DELIM + "loose_media.bin";
	std::ofstream os(loose_file, std::ios::binary);
	os << "loose media bytes";
	os.close();
	const std::string loose_hash = hashing::sha1("loose media bytes");
	fs::DeleteSingleFileOrEmptyDirectory(cache_dir + DIR_DELIM + hex_encode(loose_hash));

	UASSERT(clientMediaUpdateCacheCopy(loose_hash, loose_file));
	UASSERT(fs::PathExistsNative(cache_dir + DIR_DELIM + hex_encode(loose_hash)));
	fs::DeleteSingleFileOrEmptyDirectory(cache_dir + DIR_DELIM + hex_encode(loose_hash));
#endif
}

void TestContentVFS::testCompareVersions()
{
	using V = ContentVFS;
	UASSERT(V::compareVersions("1.4.10", "1.4.9") > 0);   // numeric, not lexical
	UASSERT(V::compareVersions("1.4.9", "1.4.10") < 0);
	UASSERT(V::compareVersions("1.4", "1.4.0") == 0);      // missing component = 0
	UASSERT(V::compareVersions("1.4.0.1", "1.4") > 0);
	UASSERT(V::compareVersions("2", "1.99.99") > 0);
	UASSERT(V::compareVersions("1.4.2-rc1", "1.4.2") == 0); // suffix ignored
	UASSERT(V::compareVersions("1.4.2", "1.4.2") == 0);
	UASSERT(V::compareVersions("", "") == 0);
	UASSERT(V::compareVersions("", "0.0.1") < 0);
}

void TestContentVFS::testMinAppVersion()
{
	std::string err;
	PackOpts o;
	o.mount_spec = "user:/__vfs_minapp__";
	o.id = "vfsminapp";
	o.type = "content";
	o.min_app_version = "1.5.0";
	const std::string path = makePack(o);

	// No app version configured (desktop, dev): the gate is off
	ContentVFS::get().setAppVersion("");
	UASSERT(ContentVFS::get().mountPackFile(path, ContentVFS::Source::Bundled, err));
	ContentVFS::get().unmountAll();

	// Older app: refused with a message naming both versions
	ContentVFS::get().setAppVersion("1.4.9");
	UASSERT(!ContentVFS::get().mountPackFile(path, ContentVFS::Source::Bundled, err));
	UASSERT(err.find("1.5.0") != std::string::npos);
	UASSERT(err.find("1.4.9") != std::string::npos);

	// Equal and newer apps: fine
	ContentVFS::get().setAppVersion("1.5.0");
	UASSERT(ContentVFS::get().mountPackFile(path, ContentVFS::Source::Bundled, err));
	ContentVFS::get().unmountAll();
	ContentVFS::get().setAppVersion("2.0");
	UASSERT(ContentVFS::get().mountPackFile(path, ContentVFS::Source::Bundled, err));
	ContentVFS::get().unmountAll();

	ContentVFS::get().setAppVersion("");
	// re-mount the shared fixture for the tests that follow
	UASSERT(ContentVFS::get().mountPackFile(m_pack_path, ContentVFS::Source::Bundled, err));
}

void TestContentVFS::testInstalledPacksSync()
{
	// The installed directory is what the delivery layer writes to; the
	// engine reconciles it on every main-menu session.
	const std::string prefix = porting::path_user + DIR_DELIM + "__vfs_inst__";
	const std::string file = m_installed_dir + DIR_DELIM + "vfsinst.kpk";

	// Empty directory: nothing happens
	ContentVFS::get().syncInstalledPacks();
	UASSERT(!ContentVFS::get().getPack("vfsinst"));

	// v1 appears -> mounted from Installed
	PackOpts v1;
	v1.mount_spec = "user:/__vfs_inst__";
	v1.id = "vfsinst";
	v1.type = "content";
	v1.version = 1;
	v1.a_content = "content v1";
	v1.out_dir = m_installed_dir;
	makePack(v1);
	ContentVFS::get().syncInstalledPacks();
	auto pack = ContentVFS::get().getPack("vfsinst");
	UASSERT(pack && pack->version() == 1);
	std::string content;
	UASSERT(fs::ReadFile(prefix + DIR_DELIM + "a.txt", content, false));
	UASSERTEQ(std::string, content, "content v1");
	bool from_installed = false;
	for (const auto &m : ContentVFS::get().getMounts())
		if (m.pack->id() == "vfsinst")
			from_installed = m.source == ContentVFS::Source::Installed;
	UASSERT(from_installed);

	// Nothing changed: sync is a no-op (same pack object survives)
	ContentVFS::get().syncInstalledPacks();
	UASSERT(ContentVFS::get().getPack("vfsinst").get() == pack.get());

	// The delivery layer swaps in v2 under the same name (atomic rename):
	// the next sync serves the new content
	std::this_thread::sleep_for(std::chrono::milliseconds(1100)); // distinct mtime
	PackOpts v2 = v1;
	v2.version = 2;
	v2.a_content = "content v2";
	makePack(v2);
	ContentVFS::get().syncInstalledPacks();
	pack = ContentVFS::get().getPack("vfsinst");
	UASSERT(pack && pack->version() == 2);
	UASSERT(fs::ReadFile(prefix + DIR_DELIM + "a.txt", content, false));
	UASSERTEQ(std::string, content, "content v2");

	// A "base" pack (bytecode-capable) dropped into the installed directory
	// is refused, and the refusal does not disturb the rest
	PackOpts base;
	base.mount_spec = "user:/__vfs_inst_base__";
	base.id = "vfsinstbase";
	base.type = "base";
	base.out_dir = m_installed_dir;
	makePack(base);
	ContentVFS::get().syncInstalledPacks();
	UASSERT(!ContentVFS::get().getPack("vfsinstbase"));
	UASSERT(ContentVFS::get().getPack("vfsinst"));

	// World packs are not mounts either
	PackOpts world;
	world.mount_spec = "user:/__vfs_inst_world__";
	world.id = "vfsinstworld";
	world.type = "world";
	world.out_dir = m_installed_dir;
	makePack(world);
	ContentVFS::get().syncInstalledPacks();
	UASSERT(!ContentVFS::get().getPack("vfsinstworld"));

	// File removed -> unmounted; the old pack object stays readable for
	// whoever still holds it (open fd), the table no longer lists it
	fs::DeleteSingleFileOrEmptyDirectory(file);
	ContentVFS::get().syncInstalledPacks();
	UASSERT(!ContentVFS::get().getPack("vfsinst"));
	UASSERT(!fs::PathExists(prefix + DIR_DELIM + "a.txt"));
#ifndef _WIN32
	UASSERT(pack->readEntry(*pack->findEntry("a.txt"), content));
	UASSERTEQ(std::string, content, "content v2");
#endif

	// Resync request handshake used by the menu loop
	UASSERT(!ContentVFS::get().consumeResyncRequest());
	ContentVFS::get().requestResync();
	UASSERT(ContentVFS::get().consumeResyncRequest());
	UASSERT(!ContentVFS::get().consumeResyncRequest());

	fs::DeleteSingleFileOrEmptyDirectory(m_installed_dir + DIR_DELIM + "vfsinstbase.kpk");
	fs::DeleteSingleFileOrEmptyDirectory(m_installed_dir + DIR_DELIM + "vfsinstworld.kpk");
	ContentVFS::get().syncInstalledPacks();
}

void TestContentVFS::testPriority()
{
	std::string err;
	const std::string prefix = porting::path_user + DIR_DELIM + "__vfs_prio__";

	// Same id, bundled v1 and installed v2: v2 is the visible one, whatever
	// the mount order.
	PackOpts b1;
	b1.mount_spec = "user:/__vfs_prio__";
	b1.id = "vfsprio";
	b1.type = "content";
	b1.version = 1;
	b1.a_content = "bundled v1";
	b1.file_name = "vfsprio_bundled.kpk";
	UASSERT(ContentVFS::get().mountPackFile(makePack(b1), ContentVFS::Source::Bundled, err));

	PackOpts i2 = b1;
	i2.version = 2;
	i2.a_content = "installed v2";
	i2.out_dir = m_installed_dir;
	i2.file_name = "vfsprio.kpk";
	makePack(i2);
	ContentVFS::get().syncInstalledPacks();

	auto pack = ContentVFS::get().getPack("vfsprio");
	UASSERT(pack && pack->version() == 2);
	std::string content;
	UASSERT(fs::ReadFile(prefix + DIR_DELIM + "a.txt", content, false));
	UASSERTEQ(std::string, content, "installed v2");
	// only one visible mount for the id
	int visible = 0;
	for (const auto &m : ContentVFS::get().getMounts())
		if (m.pack->id() == "vfsprio")
			visible++;
	UASSERTEQ(int, visible, 1);

	// The installed copy goes away: the shadowed bundled v1 takes over
	// without any remount.
	fs::DeleteSingleFileOrEmptyDirectory(m_installed_dir + DIR_DELIM + "vfsprio.kpk");
	ContentVFS::get().syncInstalledPacks();
	pack = ContentVFS::get().getPack("vfsprio");
	UASSERT(pack && pack->version() == 1);
	UASSERT(fs::ReadFile(prefix + DIR_DELIM + "a.txt", content, false));
	UASSERTEQ(std::string, content, "bundled v1");

	// Bundled v3 beats installed v2 — the newest version wins, not the
	// source (a stale download must not shadow a newer app).
	PackOpts b3 = b1;
	b3.version = 3;
	b3.a_content = "bundled v3";
	b3.file_name = "vfsprio_bundled3.kpk";
	UASSERT(ContentVFS::get().mountPackFile(makePack(b3), ContentVFS::Source::Bundled, err));
	makePack(i2);
	ContentVFS::get().syncInstalledPacks();
	pack = ContentVFS::get().getPack("vfsprio");
	UASSERT(pack && pack->version() == 3);
	UASSERT(fs::ReadFile(prefix + DIR_DELIM + "a.txt", content, false));
	UASSERTEQ(std::string, content, "bundled v3");

	// Different ids providing the same path under one prefix (a packaging
	// bug the catalog builder rejects): the installed pack is looked up
	// first, deterministically.
	PackOpts other;
	other.mount_spec = "user:/__vfs_prio__";
	other.id = "vfsprio_other";
	other.type = "content";
	other.version = 1;
	other.a_content = "other installed";
	other.out_dir = m_installed_dir;
	makePack(other);
	ContentVFS::get().syncInstalledPacks();
	UASSERT(ContentVFS::get().getPack("vfsprio_other"));
	UASSERT(fs::ReadFile(prefix + DIR_DELIM + "a.txt", content, false));
	UASSERTEQ(std::string, content, "other installed");

	// Lua-facing view: installed first, then bundled
	const auto mounts = ContentVFS::get().getMounts();
	size_t idx_other = mounts.size(), idx_prio = mounts.size();
	for (size_t i = 0; i < mounts.size(); i++) {
		if (mounts[i].pack->id() == "vfsprio_other") idx_other = i;
		if (mounts[i].pack->id() == "vfsprio") idx_prio = i;
	}
	UASSERT(idx_other < idx_prio);

	fs::DeleteSingleFileOrEmptyDirectory(m_installed_dir + DIR_DELIM + "vfsprio.kpk");
	fs::DeleteSingleFileOrEmptyDirectory(m_installed_dir + DIR_DELIM + "vfsprio_other.kpk");
	ContentVFS::get().syncInstalledPacks();
}
