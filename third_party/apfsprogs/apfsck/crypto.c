#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <apfs/aes.h>
#include <apfs/raw.h>
#include <apfs/types.h>
#include "apfsck.h"
#include "crypto.h"
#include "spaceman.h"
#include "super.h"

static void check_volume_key_entry(const u8 *keydata, u16 keylen)
{
	/*
	 * winapfs (upstream candidate): 0x28 is a bare RFC 3394 wrapped key.
	 * macOS-made software-encrypted volumes carry a DER-encoded wrapped VEK
	 * here instead (0x7c bytes in the test images; apfs-fuse decodes it), so
	 * another size is an unknown format, not corruption.
	 */
	if (keylen != 0x28)
		report_unknown("DER-encoded volume key entry in keybag");
}

static void check_volume_unlock_records_entry(const u8 *keydata, u16 keylen)
{
	struct apfs_prange *loc = NULL;
	u64 bno, blkcnt;

	if (keylen != sizeof(*loc))
		report("Volume unlock records entry in keybag", "wrong size.");
	loc = (struct apfs_prange *)keydata;

	bno = le64_to_cpu(loc->pr_start_paddr);
	blkcnt = le64_to_cpu(loc->pr_block_count);
	if (blkcnt != 1)
		report_unknown("Multiblock keybag");

	/* TODO: actually check the volume keybag */
	container_bmap_mark_as_used(bno, blkcnt);
}

/* No idea what any of this may mean, but put some checks in place */
static void check_reserved_f8_entry(const u8 *keydata, u16 keylen)
{
	u8 expected[] = "IMAKEYBAGCOOKIE!";

	if (keylen != sizeof(expected) - 1)
		report_unknown("Reserved F8 entry");
	if (memcmp(keydata, expected, keylen) != 0)
		report_unknown("Reserved F8 entry");
}

/**
 * check_keybag_entry - Check a single entry in the keybag locker
 * @entry:	the entry to check
 * @remaining:	bytes left in the locker
 *
 * Returns the length of this entry.
 */
static u16 check_keybag_entry(struct apfs_keybag_entry *entry, u16 remaining)
{
	u8 *keydata = NULL;
	u16 keylen, entry_len;

	if (remaining < sizeof(*entry))
		report("Keybag entry", "won't fit in locker.");

	keylen = le16_to_cpu(entry->ke_keylen);
	if (keylen > APFS_VOL_KEYBAG_ENTRY_MAX_SIZE - sizeof(*entry))
		report("Keybag entry", "is too big.");
	/*
	 * winapfs (upstream candidate): entries in a locker are padded to a
	 * multiple of 16 bytes. Without this, a keybag whose first key length is
	 * not a multiple of 16 (macOS-made software-encrypted volumes) has its
	 * second entry read at the wrong offset.
	 */
	entry_len = ROUND_UP(keylen + sizeof(*entry), 16);
	if (entry_len > remaining)
		report("Keybag entry", "too big for allocated bytes.");

	if (entry->padding)
		report("Keybag entry", "non-zero padding.");
	keydata = entry->ke_keydata;

	switch (le16_to_cpu(entry->ke_tag)) {
	case KB_TAG_VOLUME_KEY:
		check_volume_key_entry(keydata, keylen);
		break;
	case KB_TAG_VOLUME_UNLOCK_RECORDS:
		check_volume_unlock_records_entry(keydata, keylen);
		break;
	case KB_TAG_RESERVED_F8:
		/* The reference calls this reserved but I've seen it in use */
		check_reserved_f8_entry(keydata, keylen);
		break;
	case KB_TAG_VOLUME_PASSPHRASE_HINT:
	case KB_TAG_VOLUME_M_KEY:
		report("Keybag", "volume entry in container.");
	case KB_TAG_WRAPPING_M_KEY:
		report_unknown("Wrapped media key");
		break;
	case KB_TAG_UNKNOWN:
		report("Keybag entry", "null type.");
	case KB_TAG_RESERVED_1:
		report("Keybag entry", "reserved type.");
	default:
		report("Keybag entry", "invalid type.");
	}

	return entry_len;
}

static bool keybag_entry_is_null(struct apfs_keybag_entry *entry)
{
	if (!uuid_is_null(entry->ke_uuid))
		return false;
	if (entry->ke_tag || entry->ke_keylen || entry->padding)
		return false;
	return true;
}

static void check_keybag_locker(struct apfs_kb_locker *locker)
{
	struct apfs_keybag_entry *entry = NULL;
	u16 nkeys, i;
	u32 nbytes;

	if (le16_to_cpu(locker->kl_version) != APFS_KEYBAG_VERSION)
		report("Keybag locker", "wrong version.");
	if (locker->padding)
		report("Keybag locker", "non-zero padding.");

	nkeys = le16_to_cpu(locker->kl_nkeys);
	nbytes = le32_to_cpu(locker->kl_nbytes);
	if (nbytes > sb->s_blocksize - sizeof(*locker))
		report("Keybag locker", "won't fit in block.");

	entry = (void *)&locker->kl_entries[0]; /* winapfs: kl_entries is u8[] on MSVC */
	for (i = 0; i < nkeys; ++i) {
		u16 entry_len;

		entry_len = check_keybag_entry(entry, nbytes);
		entry = (void *)((char *)entry + entry_len);
		nbytes -= entry_len;
	}

	/*
	 * winapfs (upstream candidate): kl_nbytes counts the locker header plus
	 * the entries, each padded to 16 bytes, so the header size is what is
	 * left. (The 0x18 seen before was that header plus the last entry's 8
	 * bytes of padding, which the unpadded walk didn't consume.)
	 */
	if (nbytes != sizeof(*locker))
		report("Keybag locker", "bad byte count (%u left, %u keys).", nbytes, nkeys);
	if (!keybag_entry_is_null(entry))
		report("Keybag locker", "missing null termination.");
}

static void check_keybag_plaintext(void *raw)
{
	struct apfs_obj_phys *obj = raw;

	if (!obj_verify_csum(obj))
		report("Keybag header", "bad checksum.");

	/* Keybag objects are special: not physical/virtual/ephemeral */
	if (obj->o_oid)
		report("Keybag header", "has object id.");
	if (le32_to_cpu(obj->o_type) != 0x6b657973) /* "syek" */
		report("Keybag header", "wrong type.");
	if (obj->o_subtype != 0)
		report("Keybag header", "wrong subtype.");

	if (!obj->o_xid || le64_to_cpu(obj->o_xid) > sb->s_xid)
		report("Keybag header", "bad transaction id.");

	check_keybag_locker((void *)((char *)raw + sizeof(*obj)));
}

static void check_keybag_plaintext_block(u64 bno)
{
	u8 *plain = NULL;

	plain = apfs_mmap(NULL, sb->s_blocksize, PROT_READ, MAP_PRIVATE, bno * sb->s_blocksize);
	if (plain == MAP_FAILED)
		system_error();

	check_keybag_plaintext(plain);

	munmap(plain, sb->s_blocksize);
	plain = NULL;
}

/*
 * The reference claims that the keybag is wrapped with RFC3394, but it actually
 * uses AES in XTS mode. I got this information from the apfs-fuse sources, so
 * credit to Simon Gander <https://github.com/sgan81/apfs-fuse> for figuring it
 * out.
 */
static void check_keybag_ciphertext_block(u64 bno)
{
	u8 *uuid = (u8 *)sb->s_raw->nx_uuid;
	u8 *cipher = NULL, *plain = NULL;
	u64 sector;

	/*
	 * The sector number is used for the XTS tweak value. Sectors are
	 * always 512 bytes.
	 */
	sector = bno * (sb->s_blocksize / 0x200);

	cipher = apfs_mmap(NULL, sb->s_blocksize, PROT_READ, MAP_PRIVATE, bno * sb->s_blocksize);
	if (cipher == MAP_FAILED)
		system_error();

	plain = calloc(1, sb->s_blocksize);
	if (!plain)
		system_error();

	if (aes_xts_decrypt(uuid, uuid, sector, cipher, sb->s_blocksize, plain))
		report("Container keybag", "decryption failed.");
	check_keybag_plaintext(plain);

	free(plain);
	plain = NULL;
	munmap(cipher, sb->s_blocksize);
	cipher = NULL;
}

static bool is_plaintext_obj(u64 bno)
{
	void *raw = NULL;
	bool ret;

	raw = apfs_mmap(NULL, sb->s_blocksize, PROT_READ, MAP_PRIVATE, bno * sb->s_blocksize);
	if (raw == MAP_FAILED)
		system_error();

	/* It's impossible for the 64-bit checksum to randomly match */
	ret = obj_verify_csum(raw);

	munmap(raw, sb->s_blocksize);
	return ret;
}

void check_keybag(u64 bno, u64 blkcnt)
{
	/* TODO: do all containers have a keybag? */
	if (!bno) {
		if (blkcnt)
			report("Container keybag", "zero length.");
		return;
	}

	if (blkcnt != 1)
		report_unknown("Multiblock keybag");

	/*
	 * I've encountered iOS images with an unencrypted keybag. I have no
	 * idea if this is reported elsewhere (TODO), so for now just check if
	 * it looks like plaintext or not.
	 */
	if (is_plaintext_obj(bno))
		check_keybag_plaintext_block(bno);
	else
		check_keybag_ciphertext_block(bno);

	container_bmap_mark_as_used(bno, blkcnt);
}
