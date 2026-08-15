// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#include "test.h"

#include <cstring>
#include <fstream>
#include <sstream>

#include "aes_ctr.h"
#include "content_vfs.h"
#include "filesys.h"
#include "log.h"
#include "porting.h"

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
	void testRealFileWins();
	void testBytecodeTrust();
	void testEncryptedPack();

private:
	std::string makePack(const std::string &mount_spec,
			const std::string &pack_id = "vfstest",
			const std::string &pack_type = "base",
			const std::string &luajit_version = "",
			int first_entry_flags = 0,
			const unsigned char *aes_key = nullptr,
			u32 key_version = 0);
	std::string m_pack_path;
	std::string m_mount_prefix; // resolved absolute prefix
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
std::string TestContentVFS::makePack(const std::string &mount_spec,
		const std::string &pack_id, const std::string &pack_type,
		const std::string &luajit_version, int first_entry_flags,
		const unsigned char *aes_key, u32 key_version)
{
	std::string a_content = "hello from pack";
	std::string b_content = std::string("\x01\x02\x03", 3);

	unsigned char nonce_a[16], nonce_b[16], nonce_index[16];
	for (int i = 0; i < 16; i++) {
		nonce_a[i] = (unsigned char)(0xA0 + i);
		nonce_b[i] = (unsigned char)(0xB0 + i);
		nonce_index[i] = (unsigned char)(0xC0 + i);
	}
	aes256_key kctx;
	if (aes_key) {
		aes256_set_key(&kctx, aes_key);
		aes256_ctr_xor(&kctx, nonce_a, (const uint8_t *)a_content.data(),
				(uint8_t *)&a_content[0], a_content.size());
		aes256_ctr_xor(&kctx, nonce_b, (const uint8_t *)b_content.data(),
				(uint8_t *)&b_content[0], b_content.size());
	}
	const std::string n_a = aes_key ? toHex(nonce_a, 16) : std::string(32, '0');
	const std::string n_b = aes_key ? toHex(nonce_b, 16) : std::string(32, '0');

	const std::string luajit_field = luajit_version.empty() ? ""
			: ",\"luajitVersion\":\"" + luajit_version + "\"";
	const std::string meta = "{\"id\":\"" + pack_id + "\",\"mount\":\"" + mount_spec +
			"\",\"type\":\"" + pack_type + "\",\"version\":1" + luajit_field + "}";

	const u64 meta_offset = 80;
	const u64 blob_start = meta_offset + meta.size();

	// entries in index (sorted by path): a.txt, e.txt (empty), sub/b.bin
	std::ostringstream index_ss;
	index_ss << "{\"files\":["
		<< "{\"p\":\"a.txt\",\"o\":" << blob_start
		<< ",\"s\":" << a_content.size() << ",\"r\":" << a_content.size()
		<< ",\"h\":\"0000000000000000000000000000000000000000\",\"n\":\""
		<< n_a << "\",\"f\":" << first_entry_flags << "},"
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
	if (aes_key) {
		aes256_ctr_xor(&kctx, nonce_index, (const uint8_t *)index.data(),
				(uint8_t *)&index[0], index.size());
	}

	std::string blob;
	blob += "KPK1";
	putLEU32(blob, 1);  // format version
	putLEU32(blob, aes_key ? 1 : 0);  // flags: ENCRYPTED bit; index not compressed
	putLEU32(blob, aes_key ? key_version : 0);  // key version
	putLEU64(blob, meta_offset);
	putLEU64(blob, meta.size());
	putLEU64(blob, 0);  // preview offset
	putLEU64(blob, 0);  // preview size
	putLEU64(blob, index_offset);
	putLEU64(blob, index.size());
	blob += aes_key ? std::string((const char *)nonce_index, 16)
			: std::string(16, '\0'); // index nonce
	blob += meta;
	blob += a_content;
	blob += b_content;
	blob += index;

	const std::string path = getTestTempDirectory() + DIR_DELIM + pack_id + ".kpk";
	std::ofstream os(path, std::ios::binary);
	os << blob;
	os.close();
	return path;
}

void TestContentVFS::runTests(IGameDef *gamedef)
{
	// Unique virtual prefix under path_user; nothing real exists there.
	const std::string mount_spec = "user:/__vfs_selftest__";
	m_mount_prefix = porting::path_user + DIR_DELIM + "__vfs_selftest__";
	m_pack_path = makePack(mount_spec);

	TEST(testRejectsGarbage);
	TEST(testMountAndStat);
	TEST(testReadThroughFs);
	TEST(testDirListingUnion);
	TEST(testRealFileWins);
	TEST(testBytecodeTrust);
	TEST(testEncryptedPack);

	ContentVFS::get().unmountAll();
	ContentVFS::get().setKeyProvider(nullptr);
	fs::RecursiveDelete(m_mount_prefix);
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
	UASSERT(!ContentVFS::get().mountPackFile(bad_path, err));
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

	UASSERT(!ContentVFS::get().mountPackFile(evil_path, err));
}

void TestContentVFS::testMountAndStat()
{
	std::string err;
	bool mounted = ContentVFS::get().mountPackFile(m_pack_path, err);
	if (!mounted)
		errorstream << "mountPackFile: " << err << std::endl;
	UASSERT(mounted);
	UASSERT(ContentVFS::get().isActive());
	UASSERT(ContentVFS::get().getPack("vfstest") != nullptr);

	// duplicate id is refused
	UASSERT(!ContentVFS::get().mountPackFile(m_pack_path, err));

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
	const std::string stale = makePack("user:/__vfs_stale__", "vfsstale",
			"base", "0.0.bogus");
	UASSERT(!ContentVFS::get().mountPackFile(stale, err));
	UASSERT(err.find("LuaJIT") != std::string::npos);

	// Bytecode entries without a recorded luajitVersion cannot be
	// version-gated — the mount must be refused too.
	const std::string unversioned = makePack("user:/__vfs_nolj__", "vfsnolj",
			"base", "", ContentPack::EFLAG_LUA_BYTECODE);
	UASSERT(!ContentVFS::get().mountPackFile(unversioned, err));
	UASSERT(err.find("luajitVersion") != std::string::npos);

	// Entries of the mounted "base" pack are trusted code paths...
	const std::string trusted_file = m_mount_prefix + DIR_DELIM + "a.txt";
	UASSERT(ContentVFS::get().isTrustedCodePath(trusted_file));
	// ...but not directories, absent entries or uncovered paths
	UASSERT(!ContentVFS::get().isTrustedCodePath(m_mount_prefix + DIR_DELIM + "sub"));
	UASSERT(!ContentVFS::get().isTrustedCodePath(m_mount_prefix + DIR_DELIM + "nope"));
	UASSERT(!ContentVFS::get().isTrustedCodePath(porting::path_user));

	// A downloadable ("content") pack never grants code trust
	const std::string data_pack = makePack("user:/__vfs_data__", "vfsdata",
			"content", "");
	UASSERT(ContentVFS::get().mountPackFile(data_pack, err));
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
	const std::string enc = makePack("user:/__vfs_enc__", "vfsenc", "base", "",
			0, TEST_AES_KEY, 7);

	// No provider installed: encrypted packs are refused outright
	ContentVFS::get().setKeyProvider(nullptr);
	UASSERT(!ContentVFS::get().mountPackFile(enc, err));
	UASSERT(err.find("key provider") != std::string::npos);

	// Provider that does not know the pack's key_version
	ContentVFS::get().setKeyProvider(testKeyProvider);
	const std::string enc_v9 = makePack("user:/__vfs_enc9__", "vfsenc9", "base", "",
			0, TEST_AES_KEY, 9);
	UASSERT(!ContentVFS::get().mountPackFile(enc_v9, err));
	UASSERT(err.find("key_version 9") != std::string::npos);

	// Right key: mounts, and content decrypts through the plain fs:: API
	UASSERT(ContentVFS::get().mountPackFile(enc, err));
	const ContentPack *pack = ContentVFS::get().getPack("vfsenc");
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
