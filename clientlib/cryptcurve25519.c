#include <glib/gstdio.h>
// #include <glib.h>
/**
 * @file
 * @brief Implements the @ref CryptCurve25519 class - A Frame for encrypting packets
 * @details It uses <i>libsodium</i> to implement public key encryption in packets.
 *
 * This file is part of the Assimilation Project.
 *
 * @author Copyright &copy; 2011, 2012 - Alan Robertson <alanr@unix.sh>
 * @n
 *  The Assimilation software is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  The Assimilation software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with the Assimilation Project software.  If not, see http://www.gnu.org/licenses/
 */
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <pwd.h>
#include <errno.h>
#include <string.h>
#include <projectcommon.h>
#include <frameset.h>
#include <sodium.h>
#include <misc.h>
#include <cryptcurve25519.h>
#include <frametypes.h>
#include <generic_tlv_min.h>
#include <tlvhelper.h>

DEBUGDECLARATIONS

#define EOS	'\0'
#define	KEY_NAMING_CHECKSUM	G_CHECKSUM_MD5

///@defgroup CryptCurve25519 CryptCurve25519 class
/// Class for encrypting FrameSets.
/// @{
/// @ingroup CryptFrame


FSTATIC void _cryptcurve25519_finalize(AssimObj* aobj);
FSTATIC gboolean _cryptcurve25519_default_isvalid(const Frame *, gconstpointer, gconstpointer);
FSTATIC void	 _cryptcurve25519_updatedata(Frame*f, gpointer tlvstart, gconstpointer pktend, FrameSet* fs);
FSTATIC gboolean _is_valid_curve25519_key_id(const char * key_id, enum keytype ktype);
FSTATIC gboolean _is_legal_curve25519_key_id(const char * key_id);
FSTATIC char*	 _cache_curve25519_key_id_to_dirname(const char * key_id, enum keytype);
FSTATIC void	 _cryptcurve25519_make_cryptdir(const char * dirname);
FSTATIC gboolean _cache_curve25519_keypair(const char * key_id);
FSTATIC gboolean _cryptcurve25519_save_a_key(const char * key_id, enum keytype ktype, gconstpointer key);
FSTATIC enum keytype _cryptcurve25519_keytype_from_filename(const char *filename);
FSTATIC char * _cryptcurve25519_key_id_from_filename(const char *filename);
static void (*_parentclass_finalize)(AssimObj*) = NULL;
FSTATIC void dump_memory(const char * label, const guint8* start, const guint8* end);
FSTATIC char* cryptcurve25519_naming_checksum(const guint8* buf, size_t buflen);
FSTATIC void cryptcurve25519_debug_checksum(const char * function, int lineno, const char * message, const guint8* buf, size_t bufsize);
#define DEBUGCKSUM2(msg, buf, bufsize) {if (DEBUG >= 2) cryptcurve25519_debug_checksum(__FUNCTION__, __LINE__, msg, buf, bufsize);}
#define DEBUGCKSUM3(msg, buf, bufsize) {if (DEBUG >= 3) cryptcurve25519_debug_checksum(__FUNCTION__, __LINE__, msg, buf, bufsize);}
#define DEBUGCKSUM4(msg, buf, bufsize) {if (DEBUG >= 4) cryptcurve25519_debug_checksum(__FUNCTION__, __LINE__, msg, buf, bufsize);}

// Simple memory dump routine
FSTATIC void
dump_memory(const char * label, const guint8* start, const guint8* end)
{
	GString*		gs = g_string_new(NULL);
	const guint8*		p;

	for (p=start; p < end; ++p) {
		g_string_append_printf(gs, " %02x", (unsigned char)*p);
	}
	g_info("%s [%ld bytes]%s", label, (long)(end - start), gs->str);
	g_string_free(gs, TRUE);
	gs = NULL;
}

/*
  Our CryptCurve25519 Frame (our TLV Value) looks like this on the wire
  +----------+---------+----------+----------+-----------------------+---------------------+------------+
  | sender  |  sender  | receiver | receiver |                       |                     |            |
  | key_id  |  key id  | key name |  key id  | crypto_box_NONCEBYTES | crypto_box_MACBYTES | cyphertext |
  | length  |          |  length  |          | (randomeness - nonce) |  MAC for cyphertext |     --     | 
  |         |("length" |          |("length" |                       |                     | originally |
  | (1 byte)|  bytes)  | (1 byte) |  bytes)  |                       |                     | many frames|
  +----------+---------+----------+----------+-----------------------+---------------------+------------+
  |<---------------------------- length() value in memory -------------------------------->|
  |<------------------------------- TLV length on the wire -------------------------------------------->|
  For the sender:   the sender key is private, and the receiver key is public
  For the receiver: the sender key is public,  and the receiver key is private
 */
// Since we allocate enough space for everything in advance, we can do encryption in place
#define	TLVLEN(receiverkey_id, senderkey_id) 		\
	(4 + strnlen(receiverkey_id, MAXCRYPTNAMELENGTH+1) + strnlen(senderkey_id, MAXCRYPTNAMELENGTH+1) \
	+	crypto_box_NONCEBYTES + crypto_box_MACBYTES)

/// Map a key name on the wire to a file name in the filesystem
/// We make this a function on the idea that we might eventually want to have hashed subdirectories
/// or something similar...
/// Given how we structure the nanoprobe names, using the last three characters of the filename as the directory
/// name would be a win.  That would give us around 4096 subdirectories for the total.  Of course, this only makes
/// sense if you're going to have many more than 40K files (systems*2) to manage.

FSTATIC char*
_cache_curve25519_key_id_to_dirname(const char * key_id,	///< key_id to convert to a filename
				     enum keytype ktype)	///< Which type of key?
{
	(void)key_id;
	(void)ktype;
	return g_strdup(CRYPTKEYDIR);
}

WINEXPORT char*
curve25519_key_id_to_filename(const char * key_id,	///< key_id to convert to a filename
				     enum keytype ktype)	///< Which type of key?
{
	char *		dirname = _cache_curve25519_key_id_to_dirname(key_id, ktype);
	const char *	suffix = (PRIVATEKEY == ktype ? PRIVATEKEYSUFFIX : PUBKEYSUFFIX);
	char*		ret;
	ret = g_strdup_printf("%s%s%s%s", dirname, DIRDELIM, key_id, suffix);
	FREE(dirname);
	return ret;
}

/// @ref CryptCurve25519 function to check if a given curve25519 key id is properly formatted
/// This name might come from a bad guy, so let's carefully scrub the name
FSTATIC gboolean
_is_legal_curve25519_key_id(const char * key_id)	///< Key id to validate
{
	static const char *	validchars =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.+@#";
	int			length;
	for (length=0; length < MAXCRYPTKEYNAMELENGTH && key_id[length] != EOS; ++length) {
		if (strchr(validchars, key_id[length]) == NULL) {
			return FALSE;
		}
	}
	if (length > MAXCRYPTKEYNAMELENGTH) {
		return FALSE;
	}
	return TRUE;
}


/// Determine the type of key this might be according to its filename
FSTATIC enum keytype
_cryptcurve25519_keytype_from_filename(const char *filename)	///< Filename to classify
{
	gsize	filenamesize = strlen(filename)+1;

	if (filenamesize > sizeof(PUBKEYSUFFIX)) {
		gsize offset = filenamesize - sizeof(PUBKEYSUFFIX);
		if (strcmp(filename+offset, PUBKEYSUFFIX) == 0) {
			return PUBLICKEY;
		}
	}
	if (filenamesize > sizeof(PRIVATEKEYSUFFIX)) {
		gsize offset = filenamesize - sizeof(PRIVATEKEYSUFFIX);
		if (strcmp(filename+offset, PRIVATEKEYSUFFIX) == 0) {
			return PRIVATEKEY;
		}
	}
	return NOTAKEY;
}

/// Determine the key_id this might is according to its pathname
FSTATIC char *
_cryptcurve25519_key_id_from_filename(const char *filename)	///< filename to examine
{
	enum keytype	ktype = _cryptcurve25519_keytype_from_filename(filename);
	gsize		filenamesize;
	gsize		suffixlen;
	gsize		prefixlen;
	const char*	lastslash;
	gsize		idlen;
	char *		key_id;

	if (ktype == NOTAKEY) {
		return NULL;
	}
	filenamesize = strlen(filename);
	suffixlen = (ktype == PRIVATEKEY ? sizeof(PRIVATEKEYSUFFIX) : sizeof(PUBKEYSUFFIX))-1;
	lastslash = strrchr(filename, DIRDELIM[0]);
	prefixlen = (lastslash == NULL ? 0 : (1+ (lastslash - filename)));
	idlen = (filenamesize - prefixlen) - suffixlen;
	key_id = g_strndup(filename + prefixlen, idlen);
	if (!_is_legal_curve25519_key_id(key_id)) {
		g_free(key_id);  key_id = NULL;
	}
	return key_id;
}



/// @ref CryptCurve25519 function to check if a given curve25519 key id is valid
/// This name might come from a bad guy, so let's carefully scrub the name
FSTATIC gboolean
_is_valid_curve25519_key_id(const char * key_id,	///< key_id to validate
			     enum keytype ktype)	///< which kind of key is it?
{
	if (!_is_legal_curve25519_key_id(key_id)) {
		g_warning("%s.%d: Key id %s is not legal.", __FUNCTION__, __LINE__, key_id);
		return FALSE;
	}
	if (_cache_curve25519_keypair(key_id)) {
		if (ktype == PRIVATEKEY) {
			CryptFramePrivateKey*	secret = cryptframe_private_key_by_id(key_id);
			if (NULL == secret) {
				g_warning("%s.%d: Private Key id %s is not cached."
				,	__FUNCTION__, __LINE__, key_id);
				return FALSE;
			}
		}
		return TRUE;
	}
	g_warning("%s.%d: Key id %s could not be cached.", __FUNCTION__, __LINE__, key_id);
	return FALSE;
}

/// Validate and cache the requested curve25519 keypair (or just public if no private)
/// If it's already in memory (like a temporary key) we won't look for it on disk.
FSTATIC gboolean
_cache_curve25519_keypair(const char * key_id)	///< Key id of keypair to cache
{
	GStatBuf	statinfo;
	char *		filename;
	gpointer	public_key = NULL;
	gpointer	secret_key = NULL;
	gboolean	retval = TRUE;
	int		fd = -1;
	int		rc;
	
	/*
	 *	Coverity CID 1262413:
	 *	Coverity complains about this code with a Time of Check / Time of Use warning.
	 *	The file attributes we check in the stat calls below might have changed before
	 *	we read the key files.
	 *
	 *	Although this is true, it is unlikely because of the permissions of the
	 *	directories and files involved.
	 *
	 *	We only look at two attributes:
	 *	 - file size
	 *	 - file type
	 *	If the file size changes, we will detect that because we try and read one extra byte.
	 *	If the file type changes, we're screwed.  But that's really unlikely and
	 *	unlikely to have the code succeed when reading a socket, directory, fifo or device.
	 *	It might cause the program to hang (on read), but I think that's nearly inevitable
	 *	and not worth the trouble to fix.
	 *	If the file type changes, the open might hang too...
	 *	So, we're pretty paranoid, and I judge us to be at least as paranoid as necessary.
	 */
	
	if (cryptframe_public_key_by_id(key_id) != NULL) {
		return TRUE;
	}
	filename = curve25519_key_id_to_filename(key_id, PUBLICKEY);
	if (g_stat(filename, &statinfo) < 0) {
		g_warning("%s.%d: g_stat error [%s] NOT Caching key id %s [%s]"
		,	__FUNCTION__, __LINE__
		,	filename, key_id, g_strerror(errno));
		retval = FALSE;
		goto getout;
	}
	if (statinfo.st_size != crypto_box_PUBLICKEYBYTES || !S_ISREG(statinfo.st_mode)
	||	g_access(filename, R_OK) != 0) {
		retval = FALSE;
		g_warning("%s.%d: size/type/access error on %s NOT Caching key id %s"
		,	__FUNCTION__, __LINE__, filename, key_id);
		goto getout;
	}
	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		retval = FALSE;
		g_warning("%s.%d: open error on %s NOT Caching key id %s", __FUNCTION__, __LINE__
		,	filename, key_id);
		goto getout;
	}
	public_key = g_malloc(crypto_box_PUBLICKEYBYTES+1);
	rc = read(fd, public_key, crypto_box_PUBLICKEYBYTES+1);
	if (rc != crypto_box_PUBLICKEYBYTES) {
		g_warning("%s.%d: public key read on %s returned %d instead of %d [%s]"
		,	__FUNCTION__, __LINE__, filename
		,	rc, crypto_box_PUBLICKEYBYTES, g_strerror(errno));
		retval = FALSE;
		goto getout;
	}

	close(fd); fd = -1;
	DEBUGCKSUM4(filename, public_key, crypto_box_PUBLICKEYBYTES);

	g_free(filename);
	filename = curve25519_key_id_to_filename(key_id, PRIVATEKEY);
	if (g_stat(filename, &statinfo) >= 0) {
		if (statinfo.st_size != crypto_box_SECRETKEYBYTES || !S_ISREG(statinfo.st_mode)) {
			g_warning("%s.%d: secret key stat on [%s] returned %d instead of %d [%s]"
			,	__FUNCTION__, __LINE__, filename
			,	(int)statinfo.st_size, crypto_box_SECRETKEYBYTES, g_strerror(errno));
			goto getout;
		}
		if (g_access(filename, R_OK) != 0) {
			// Someone else's secret key... Not a problem...
			goto getout;
		}
		secret_key = g_malloc(crypto_box_SECRETKEYBYTES+1);
		fd = open(filename, O_RDONLY);
		if (fd < 0) {
			retval = FALSE;
			g_warning("%s.%d: open error on %s NOT Caching key id %s", __FUNCTION__, __LINE__
			,	filename, key_id);
			goto getout;
		}
		rc = read(fd, secret_key, crypto_box_SECRETKEYBYTES+1);
		if (rc != crypto_box_SECRETKEYBYTES) {
			g_warning("%s.%d: secret key read of %s returned %d instead of %d [%s]"
			,	__FUNCTION__, __LINE__, filename
			,	rc, crypto_box_SECRETKEYBYTES, g_strerror(errno));
			retval = FALSE;
			goto getout;
		}
		DEBUGCKSUM4(filename, secret_key, crypto_box_SECRETKEYBYTES);
		close(fd); fd = -1;
	}
getout:
	// Coverity: CID 1262410 - It doesn't like verifying non-NULL in this error leg
	// But it's a goto error leg, and in the future the code might change.
	// So I like it as is...
	if (filename != NULL) {
		g_free(filename);
		filename = NULL;
	}
	if (fd >= 0) {
		close(fd); fd = -1;
	}
	if (retval) {
		g_assert(public_key != NULL);
		(void)cryptframe_publickey_new(key_id, public_key);
		if (secret_key) {
			(void)cryptframe_privatekey_new(key_id, secret_key);
		}
	}else{
		if (public_key) {
			g_free(public_key);
			public_key = NULL;
		}
		if (secret_key) {
			g_free(secret_key);
			secret_key = NULL;
		}
	}
	return retval;
}

/// Purge a cryptcurve25519 key from the filesystem and from memory.
/// <i>This will modify the filesystem</i>.  It will return success if at the end of the
/// call the files do not exist, and regardless of the return value it will purge them
/// from our in-memory key cache
///
WINEXPORT gboolean 
cryptcurve25519_purge_keypair(const char* key_id)	///< Key id of keypair to purge
{
	char *		filename;
	gboolean	retval = TRUE;
	g_return_val_if_fail(_is_legal_curve25519_key_id(key_id), FALSE);

	filename = curve25519_key_id_to_filename(key_id, PUBLICKEY);
	if (g_access(filename, F_OK) == 0) {
		if (g_unlink(filename) != 0) {
			g_warning("%s.%d: Unable to remove public key file [%s]. Reason: %s"
			,	__FUNCTION__, __LINE__, filename, g_strerror(errno));
			retval = FALSE;
		}
	}
	g_free(filename); filename = NULL;

	filename = curve25519_key_id_to_filename(key_id, PRIVATEKEY);
	if (g_access(filename, F_OK) == 0) {
		if (g_unlink(filename) != 0) {
			g_warning("%s.%d: Unable to remove private key file [%s] Reason: %s"
			,	__FUNCTION__, __LINE__, filename, g_strerror(errno));
			retval = FALSE;
		}
	}
	g_free(filename); filename = NULL;
	cryptframe_purge_key_id(key_id);
	g_warning("%s.%d:  Key ID %s has been purged.", __FUNCTION__, __LINE__, key_id);
	return retval;
}

/// We read in and cache all the key pairs (or public keys) that we find in CRYPTKEYDIR
WINEXPORT void
cryptcurve25519_cache_all_keypairs(void)
{
	GDir*		key_directory;
	const char*	filename;

	_cryptcurve25519_make_cryptdir(CRYPTKEYDIR);
	key_directory = g_dir_open(CRYPTKEYDIR, 0, NULL);

	if (NULL == key_directory) {
		g_warning("%s.%d: Cannot open directory \"%s\" [%s]", __FUNCTION__, __LINE__
		,	CRYPTKEYDIR, g_strerror(errno));
		return;
	}

	while (NULL != (filename = g_dir_read_name(key_directory))) {
		if (_cryptcurve25519_keytype_from_filename(filename) == PUBLICKEY) {
			char *	key_id = _cryptcurve25519_key_id_from_filename(filename);
			if (NULL == key_id) {
				continue;
			}
			_cache_curve25519_keypair(key_id);
			g_free(key_id); key_id = NULL;
		}
	}
	g_dir_close(key_directory);
}

/// @ref CryptCurve25519 'isvalid' member function (checks for valid cryptcurve25519 objects)
FSTATIC gboolean
_cryptcurve25519_default_isvalid(const Frame * fself,	///<[in] CryptCurve25519 object ('this')
			      gconstpointer tlvstart,	///<[in] Pointer to the TLV for this CryptCurve25519
			      gconstpointer pktend)	///<[in] Pointer to one byte past the end of the packet
{
	const CryptCurve25519*	self = CASTTOCONSTCLASS(CryptCurve25519, fself);
	const guint8*	valptr;
	const char*	key_id;
	guint		namelen;
	gsize		pktlen;
	int		j;

	// Validate "object" only
	if (NULL == tlvstart) {
		namelen = strnlen(self->baseclass.receiver_key_id, MAXCRYPTNAMELENGTH+1);
		if (fself->length != TLVLEN(self->baseclass.receiver_key_id, self->baseclass.sender_key_id)) {
			g_warning("%s.%d: Packet length [%d] is incorrect."
			,	__FUNCTION__, __LINE__, (int)fself->length);
			return FALSE;
		}
		namelen = strnlen(self->baseclass.receiver_key_id, MAXCRYPTNAMELENGTH+1);
		if (namelen >= MAXCRYPTNAMELENGTH || namelen < 1 ){
			g_warning("%s.%d: Packet length [%d] is invalid."
			,	__FUNCTION__, __LINE__, (int)namelen);
			return FALSE;
		}
		if (!_is_valid_curve25519_key_id(self->baseclass.receiver_key_id, self->forsending? PUBLICKEY: PRIVATEKEY)) {
			g_warning("%s.%d: %s ID is not a valid curve25519 key ID."
			,	__FUNCTION__, __LINE__, self->forsending ? "PUBLICKEY": "PRIVATEKEY");
			return FALSE;
		}
		namelen = strnlen(self->baseclass.sender_key_id, MAXCRYPTNAMELENGTH+1);
		if (namelen >= MAXCRYPTNAMELENGTH || namelen < 1 ){
			g_warning("%s.%d: key id length (%d) is invalid"
			,	__FUNCTION__, __LINE__, namelen);
			return FALSE;
		}
		if (!_is_valid_curve25519_key_id(self->baseclass.sender_key_id, self->forsending ? PRIVATEKEY: PUBLICKEY)) {
			g_warning("%s.%d: %s ID is not a valid curve25519 key ID."
			,	__FUNCTION__, __LINE__, self->forsending ? "PRIVATEKEY" : "PUBLICKEY");
			return FALSE;
		}
		return TRUE;
	}

	// Validate TLV
	pktlen = get_generic_tlv_len(tlvstart, pktend);
	// 6 == two 1-byte lengths, and two NUL-terminated strings of at least 2 bytes each
	if (pktlen < (crypto_box_NONCEBYTES + crypto_box_MACBYTES+6)) {
		g_warning("%s.%d: Packet length [%d] is too short."
		,	__FUNCTION__, __LINE__, (int)pktlen);
		return FALSE;
	}
	valptr = get_generic_tlv_value(tlvstart, pktend);
	// Validate both key names in the packet...
	for (j=0; j < 2; ++ j) {
		if ((gconstpointer)(valptr+3) >= pktend) {
			g_warning("%s.%d: Key name (%d) extends beyond end."
			,	__FUNCTION__, __LINE__, j);
			return FALSE;
		}
		namelen = tlv_get_guint8(valptr, pktend);
		if (namelen < 2 || (namelen-1) > MAXCRYPTNAMELENGTH) {
			g_warning("%s.%d: Name (%d) length (%d) is invalid."
			,	__FUNCTION__, __LINE__, j, namelen);
			return FALSE;
		}
		valptr += 1;
		if ((gconstpointer)(valptr+namelen) > pktend) {
			g_warning("%s.%d: Key name (%d) extends beyond end", __FUNCTION__, __LINE__, j);
			return FALSE;
		}
		key_id = (const char *)(valptr);
		if (strnlen(key_id, namelen) != (namelen-1)) {
			g_warning("%s.%d: Key name (%d) length %d improperly terminated"
			,	__FUNCTION__, __LINE__, j, namelen);
			return FALSE;
		}
		// We say PUBLICKEY since we don't know whether we're validating this
		// on the sender or the receiver end - and whether should be a public
		// or a private key will depend on which end we're at - and everyone
		// needs a public key.  If we have a public key but need a private
		// key that will get caught when we try and decrypt it.
		// At least this catches garbage and unknown keys
		if (!_is_valid_curve25519_key_id(key_id, PUBLICKEY)) {
			g_warning("%s.%d: Packet encrypted using unknown key [%s]", __FUNCTION__, __LINE__
			,	key_id);
			return FALSE;
		}
		valptr += namelen;
	}
	return TRUE;
}


/// Construct a new CryptCurve25519 object (frame).
CryptCurve25519*
cryptcurve25519_new(guint16 frame_type,	///<[in] TLV type of CryptCurve25519
	  const char * sender_key_id,	///<[in] name of sender's key
	  const char * receiver_key_id,	///<[in] name of receiver's key
	  gboolean     forsending,	///<[in] TRUE if this is for sending
	  gsize objsize)		///<[in] sizeof(this object) - or zero for default
{
	CryptFrame*		baseframe;
	CryptCurve25519*	ret;

	BINDDEBUG(CryptCurve25519);
	if (objsize < sizeof(CryptCurve25519)) {
		objsize = sizeof(CryptCurve25519);
	}
	if (NULL == sender_key_id) {
		sender_key_id = cryptframe_get_signing_key_id();
	}
	DEBUGMSG2("%s.%d:(%s, %s, %d)", __FUNCTION__, __LINE__, sender_key_id, receiver_key_id
	,	(int)objsize);
	g_return_val_if_fail(sender_key_id != NULL && receiver_key_id != NULL, NULL);
	if (!_is_valid_curve25519_key_id(receiver_key_id, PUBLICKEY)) {
		g_critical("%s.%d: public key name [%s] is invalid", __FUNCTION__, __LINE__, receiver_key_id);
		return NULL;
	}
	if (!_is_valid_curve25519_key_id(sender_key_id, PUBLICKEY)) {
		g_critical("%s.%d: public key name [%s] is invalid", __FUNCTION__, __LINE__, sender_key_id);
		return NULL;
	}
	baseframe = cryptframe_new(frame_type, sender_key_id, receiver_key_id, objsize);
	if (!_parentclass_finalize) {
		_parentclass_finalize = baseframe->baseclass.baseclass._finalize;
	}
	baseframe->baseclass.isvalid	= _cryptcurve25519_default_isvalid;
	baseframe->baseclass.updatedata	= _cryptcurve25519_updatedata;
	baseframe->baseclass.length	= TLVLEN(receiver_key_id, sender_key_id);
	baseframe->baseclass.baseclass._finalize = _cryptcurve25519_finalize;
	ret			= NEWSUBCLASS(CryptCurve25519, baseframe);
	ret->forsending		= forsending;
	ret->private_key	= cryptframe_private_key_by_id(forsending ? sender_key_id : receiver_key_id);
	ret->public_key		= cryptframe_public_key_by_id(forsending ? receiver_key_id : sender_key_id);
	if (ret->private_key && ret->public_key) {
		DEBUGCKSUM3("private_key:", ret->private_key->private_key, crypto_box_SECRETKEYBYTES);
		DEBUGCKSUM3("public_key:", ret->public_key->public_key, crypto_box_PUBLICKEYBYTES);
		DUMP3(__FUNCTION__, &ret->baseclass.baseclass.baseclass, " is return value");
		REF(ret->private_key);
		REF(ret->public_key);
	}else{
		if (!ret->private_key) {
			g_warning("%s.%d: Sender private key is NULL for key id %s", __FUNCTION__, __LINE__
			,	sender_key_id);
			abort();
		}
		if (!ret->public_key) {
			g_warning("%s.%d: Receiver public key is NULL for key id %s", __FUNCTION__, __LINE__
			,	receiver_key_id);
		}
		UNREF3(ret);
		return NULL;
	}
	return ret;
}

/// Finalize (free) a CryptCurve25519 object
FSTATIC void
_cryptcurve25519_finalize(AssimObj* aself)	///< Object to finalize/free
{
	CryptCurve25519*	self = CASTTOCLASS(CryptCurve25519, aself);
	
	if (self->public_key) {
		UNREF(self->public_key);
	}
	if (self->private_key) {
		UNREF(self->private_key);
	}
	_parentclass_finalize(aself);
}

/// Given marshalled packet data corresponding to an CryptCurve25519 frame
/// return the corresponding Frame
/// In other words, un-marshall the data...
/// In our case, this means we decrypt it in-place into many other frames...
WINEXPORT Frame*
cryptcurve25519_tlvconstructor(gpointer tlvstart,	///<[in/out] Start of marshalled CStringFrame data
			  gconstpointer pktend,		///<[in] Pointer to first invalid byte past 'tlvstart'
		          gpointer* ignorednewpkt,	///<[ignored] replacement packet
		          gpointer* ignoredpktend)	///<[ignored] end of replacement packet
{
	guint8*			valptr = get_generic_tlv_nonconst_value(tlvstart, pktend);
	guint8*			nonce;
	guint8*			cyphertext;
	const guint8*		tlvend8 = valptr + get_generic_tlv_len(tlvstart, pktend);
	guint8*			plaintext;
	CryptCurve25519*	ret;
	guint			namelen;
	gsize			cypherlength;
				// The first key name is in sender's key name
				// The second key name is in receiver's key name
	CryptFramePublicKey *	sender_public_key = NULL;
	CryptFramePrivateKey*	receiver_secret_key = NULL;
	const char*		sender_pubkey_id = NULL;
	const char*		rcvr_seckey_id = NULL;
	int			j;

	(void)ignorednewpkt; (void)ignoredpktend;
	valptr = get_generic_tlv_nonconst_value(tlvstart, pktend);
	for (j=0; j < 2; ++j) {
		char *	key_id;
		g_return_val_if_fail((gpointer)(valptr+2) <= pktend, NULL);
		namelen = tlv_get_guint8(valptr, pktend);
		valptr += 1;
		g_return_val_if_fail((gpointer)(valptr+namelen) <= pktend, NULL);
		key_id = (char *)valptr;
		g_return_val_if_fail (strnlen(key_id, namelen) == namelen -1, NULL);
		g_return_val_if_fail(_is_valid_curve25519_key_id(key_id
		, 	0 == j ? PUBLICKEY : PRIVATEKEY), NULL);
		if (0 == j) {
			sender_public_key = cryptframe_public_key_by_id(key_id);
			sender_pubkey_id = key_id;
		}else{
			receiver_secret_key = cryptframe_private_key_by_id(key_id);
			rcvr_seckey_id = key_id;
		}
		g_return_val_if_fail(key_id != NULL, NULL);
		valptr += namelen;
	}
	if (NULL == sender_public_key) {
		g_warning("%s.%d: No access to sender %s public key"
		,	__FUNCTION__, __LINE__, sender_pubkey_id);
		return NULL;
	}
	if (NULL == receiver_secret_key) {
		g_warning("%s.%d: No access to receiver %s private key"
		,	__FUNCTION__, __LINE__, rcvr_seckey_id);
		return NULL;
	}
	g_return_val_if_fail((gpointer)(valptr + (crypto_box_NONCEBYTES+crypto_box_MACBYTES)) <= pktend, NULL);
	nonce = valptr;
	cyphertext = nonce + crypto_box_NONCEBYTES;
	plaintext = cyphertext + crypto_box_MACBYTES;
	cypherlength = tlvend8 - cyphertext;
	DEBUGCKSUM4("nonce:", nonce, crypto_box_NONCEBYTES);
	DEBUGCKSUM4("sender   public key :", sender_public_key -> public_key,  crypto_box_PUBLICKEYBYTES);
	DEBUGCKSUM4("receiver private key:", receiver_secret_key->private_key, crypto_box_SECRETKEYBYTES);
	DEBUGMSG4("cypher offset versus tlvstart: %ld", (long)(cyphertext-(guint8*)tlvstart));
	DEBUGCKSUM4("cypher text:", cyphertext, cypherlength);
	if (crypto_box_open_easy(plaintext, cyphertext, cypherlength, nonce
	,	sender_public_key->public_key, receiver_secret_key->private_key) != 0) {
		g_warning("%s.%d: could not decrypt %d byte message encrypted with key pair [pub:%s, sec:%s]"
		,	__FUNCTION__, __LINE__, (int)cypherlength, sender_pubkey_id, rcvr_seckey_id);
		return NULL;
	}
	DEBUGCKSUM4("plain text:", plaintext, cypherlength-crypto_box_MACBYTES);
	// Note that our return value's size will determine where the beginning of the
	// decrypted data is (according to it's dataspace() member function)
	ret = cryptcurve25519_new(get_generic_tlv_type(tlvstart, pktend)
	,	(const char *)sender_pubkey_id
	,	rcvr_seckey_id, FALSE, 0);
	return (ret ? &(ret->baseclass.baseclass) : NULL);
}
///
/// We update the data in the packet from our CryptCurve25519 object with the
/// side-effect of encrypting all the frames already put into the packet.  Note that
/// this only works because we always construct the packet from the end back to the
/// beginning.  We do this in-place - fortunately the algorithms allow that...
/// We effectively suck all the remaining frames into a single encrypted frame...
FSTATIC void	
_cryptcurve25519_updatedata(Frame* f,			///< Frame to marshall
			    gpointer tlvstart,		///< Start of our Frame in the packet
			    gconstpointer pktend,	///< Last byte in the allocated packet
			    FrameSet* unused_fs)	///< Pointer to our containing frameset
{
	CryptCurve25519*self		= CASTTOCLASS(CryptCurve25519, f);
	const guint8*	pktend8		= pktend;
	//guint8*	tlvstart8	= tlvstart;
	guint8*		tlvval;
	guint8*		valptr;
	guint32		plaintextoffset;
	guint32		plaintextsize;
	guint32		cyphertextoffset;
	guint32		nonceoffset;
	guint32		tlvsize;
	unsigned char*	nonce;
	int		j;

	(void)unused_fs;
	
	// [key1, key2, nonce, MAC, plaintext]

	DUMP3(__FUNCTION__, &f->baseclass, " is CryptCurve25519 Frame being processed.");
	DEBUGMSG3("%s.%d: tlvstart:%p, pktend:%p", __FUNCTION__, __LINE__, tlvstart, pktend);
	// The plain text starts immediately after our (incoming) frame
	plaintextoffset = f->length;					// Plain text starts here
	cyphertextoffset = plaintextoffset - crypto_box_MACBYTES;	// Preceded by MAC
	nonceoffset = cyphertextoffset - crypto_box_NONCEBYTES;		// Preceded by nonce
	// Our (outgoing) frame consists of the original incoming frame plus all other frames after ours
	tlvval = get_generic_tlv_nonconst_value(tlvstart, pktend);
	tlvsize = pktend8 - tlvval;
	plaintextsize = (tlvsize - plaintextoffset);

	// Generate a "nonce" as part of the packet - make known plaintext attacks harder
	// ... lots of our plaintext is easy to figure out ...
	nonce = tlvval + nonceoffset;
	DEBUGMSG3("%s.%d: generating random nonce (%p, %d, %p)", __FUNCTION__, __LINE__
	,	nonce, (int)crypto_box_NONCEBYTES, nonce+crypto_box_NONCEBYTES);
	randombytes_buf(nonce, crypto_box_NONCEBYTES);
	DEBUGMSG3("%s.%d: random nonce generated.", __FUNCTION__, __LINE__);

	DEBUGMSG3("%s.%d: public->key_id: [%s], private_key->key_id: [%s]", __FUNCTION__, __LINE__
	,	self->public_key->key_id, self->private_key->key_id);
	DEBUGMSG3("%s.%d: calling crypto_box_easy(%p,%p,%d,%p,%p,%p)", __FUNCTION__, __LINE__
	,	tlvval+cyphertextoffset, tlvval+plaintextoffset, plaintextsize
	,	nonce, self->public_key->public_key, self->private_key->private_key);
	DEBUGCKSUM4("plain text cksum:", tlvval+plaintextoffset, plaintextsize);
	DEBUGCKSUM4("receiver public key cksum:",self->public_key -> public_key, crypto_box_PUBLICKEYBYTES);
	DEBUGCKSUM4("sender  private key cksum:", self->private_key->private_key, crypto_box_SECRETKEYBYTES);
	DEBUGCKSUM4("nonce cksum:", nonce, crypto_box_NONCEBYTES);
	// Encrypt in-place [we previously allocated enough space for authentication info]
	g_return_if_fail(0 == crypto_box_easy(tlvval+cyphertextoffset, tlvval+plaintextoffset, plaintextsize
	,	nonce, self->public_key->public_key, self->private_key->private_key));
	DEBUGMSG4("cypher offset versus tlvstart: %ld", (long)(tlvval+cyphertextoffset-(guint8*)tlvstart));
	DEBUGCKSUM4("cypher text checksum:", tlvval+cyphertextoffset, plaintextsize+crypto_box_MACBYTES);
	set_generic_tlv_type(tlvstart, self->baseclass.baseclass.type, pktend);
	set_generic_tlv_len(tlvstart, tlvsize, pktend);
	// Put in the frame type, length, key name length, and key name for both keys
	// We're the sender - our [private] key name goes first, then the receiver's [public] key name
	valptr = get_generic_tlv_nonconst_value(tlvstart, pktend);
	for (j=0; j < 2; ++j) {
		char *	key_id = (j == 0 ? self->baseclass.sender_key_id : self->baseclass.receiver_key_id);
		int	keylen = strlen(key_id)+1;
		tlv_set_guint8(valptr, keylen, pktend);
		valptr += 1;
		g_strlcpy((char *)valptr, key_id, keylen);
		valptr += keylen;
	}
	DEBUGMSG3("%s.%d: returning after next assert (tlvval:%p, tlvsize%d, pktend:%p"
	,	__FUNCTION__, __LINE__, tlvval, (int)tlvsize, pktend);
	g_assert((tlvval + tlvsize) == pktend);
	DEBUGMSG3("%s.%d: returning (assert passed).", __FUNCTION__, __LINE__);
}

/// Generate a temporary (non-persistent) key pair
WINEXPORT void
cryptcurve25519_gen_temp_keypair(const char *key_id) ///< key_id CANNOT be NULL
{
	unsigned char*	public_key = g_malloc(crypto_box_PUBLICKEYBYTES);
	unsigned char*	secret_key = g_malloc(crypto_box_SECRETKEYBYTES);
	
	crypto_box_keypair(public_key, secret_key);
	(void) cryptframe_privatekey_new(key_id, secret_key);
	(void) cryptframe_publickey_new(key_id, public_key);
}

/// Return a malloced string containing the KEY_NAMING_CHECKSUM type checksum of the given data
FSTATIC char*
cryptcurve25519_naming_checksum(const guint8* buf,	///<[in] buffer to checksum
				size_t buflen)		///<[in] length of 'buf'
{
	return g_compute_checksum_for_data(KEY_NAMING_CHECKSUM, buf, buflen);
}

/// Print a debug checksum message
FSTATIC void
cryptcurve25519_debug_checksum( const char * function,	///<[in] function name
				int lineno,		///<[in] line number
				const char * message,	///<[in] message
				const guint8* buf,	///<[in] buffer to checksum
				size_t bufsize)		///<[in] buffer size
{
	char *	checksum = cryptcurve25519_naming_checksum(buf, bufsize);
	g_debug("%s.%d: %s [%ld]%s", function, lineno, message, (long)bufsize, checksum);
	g_free(checksum);
}

/// Create a persistent keypair and write it to disk
/// Returns a MALLOCed string with the key id for the key pair.  Please free!
WINEXPORT char *
cryptcurve25519_gen_persistent_keypair(const char * giveitaname) ///< giveitaname can be NULL
{
	unsigned char*	public_key = g_malloc(crypto_box_PUBLICKEYBYTES);
	unsigned char*	secret_key = g_malloc(crypto_box_SECRETKEYBYTES);
	char*		key_id;
	char*		sysname;
	
	// This is to try and keep our dummy array from being optimized out
	// so that we get stack protection, and clang doesn't complain...
	crypto_box_keypair(public_key, secret_key);
	if (NULL == giveitaname) {
		char*		checksum_string;
		// Then we'll generate one based on host name and key's checksum
		checksum_string = cryptcurve25519_naming_checksum(public_key, crypto_box_PUBLICKEYBYTES);
		sysname = proj_get_sysname();
		key_id = g_strdup_printf("%s@@%s", sysname, checksum_string);
		g_free(sysname); g_free(checksum_string);
	}else{
		key_id = g_strdup(giveitaname);
	}
	DEBUGMSG1("%s.%d: Generating permanent key pair [%s]", __FUNCTION__, __LINE__, key_id);
	// Write out the two generated keys (the key-pair) into the correct names
	if (!_cryptcurve25519_save_a_key(key_id, PUBLICKEY, public_key)
	||	!_cryptcurve25519_save_a_key(key_id, PRIVATEKEY, secret_key)
	||	cryptframe_privatekey_new(key_id, secret_key) == NULL
	||	cryptframe_publickey_new(key_id, public_key) == NULL) {
		// Something didn't work :-(
		cryptcurve25519_purge_keypair(key_id);
		g_free(public_key); public_key = NULL;
		g_free(secret_key); secret_key = NULL;
		g_free(key_id); key_id = NULL;
		return NULL;
	}

	_cache_curve25519_keypair(key_id);
	return key_id;
}

///Save a public key away to disk so it's completely usable...
WINEXPORT gboolean
cryptcurve25519_save_public_key(const char * key_id,	///< key id to save key under
				gpointer public_key,	///< pointer to public key data
				int keysize)		///< size of key
{
	CryptFramePublicKey*	pub;
	gpointer*	newpub;
	if (keysize != crypto_box_PUBLICKEYBYTES) {
		g_warning("%s.%d: Attempt to save a public key of %d bytes (instead of %d)"
		,	__FUNCTION__, __LINE__, keysize, crypto_box_PUBLICKEYBYTES);
		return FALSE;
	}
	if ((pub = cryptframe_public_key_by_id(key_id)) != NULL) {
		if (memcmp(public_key, pub->public_key, keysize) == 0) {
			return TRUE;
		}
		g_critical("%s.%d: Attempt to modify public key with id [%s]"
		,	__FUNCTION__, __LINE__, key_id);
		return FALSE;
	}
	newpub = malloc(keysize);
	memcpy(newpub, public_key, keysize);
	if (!_cryptcurve25519_save_a_key(key_id, PUBLICKEY, newpub)
	||	cryptframe_publickey_new(key_id, newpub) == NULL) {
		cryptcurve25519_purge_keypair(key_id);
		g_free(newpub); newpub = NULL;
		return FALSE;
	}
	if (DEBUG) {
		CryptFramePublicKey*	pub = cryptframe_public_key_by_id(key_id);
		g_return_val_if_fail(pub != NULL, FALSE);
		g_return_val_if_fail(memcmp(public_key, pub->public_key, keysize) == 0, FALSE);
	}
	return TRUE;
}


/// Make a directory for storing keys in...
FSTATIC void
_cryptcurve25519_make_cryptdir(const char * dirname)
{
	struct passwd*	pw;
	char *		cmd = g_strdup_printf("mkdir -p '%s'", dirname);
	int		rc = system(cmd);
	FREE(cmd);
	if (rc != 0) {
		g_warning("%s.%d: Could not make directory %s"
		,	__FUNCTION__, __LINE__, dirname);
	}
	rc = chmod(dirname, 0700);
	if (rc < 0) {
		g_warning("%s.%d: Could not chmod 0700 %s [%s]"
		,	__FUNCTION__, __LINE__, dirname, g_strerror(errno));
	}
	pw = getpwnam(CMAUSERID);
	if (NULL != pw) {
		rc = chown(dirname, pw->pw_uid, pw->pw_gid);
		if (rc < 0) {
			g_warning("%s.%d: Could not chown %s %s [%s]"
			,	__FUNCTION__, __LINE__, CMAUSERID, dirname, g_strerror(errno));
		}
	}
}

/// Save a curve25519 key to a file.
FSTATIC gboolean
_cryptcurve25519_save_a_key(const char * key_id,///<[in] key_id to save
			 enum keytype ktype,	///<[in] type of key being saved
			 gconstpointer key)	///<[in] pointer to key
{
	ssize_t		keysize;
	guint32		createmode;
	int		fd;
	int		rc;
	char*		filename;

	if (!_is_legal_curve25519_key_id(key_id)) {
		g_warning("%s.%d: Key id %s is illegal", __FUNCTION__, __LINE__, key_id);
		return FALSE;
	}
	filename = curve25519_key_id_to_filename(key_id, ktype);

	if (PUBLICKEY == ktype) {
		keysize = crypto_box_PUBLICKEYBYTES;
		createmode = 0644;
	}else if (PRIVATEKEY == ktype) {
		keysize = crypto_box_SECRETKEYBYTES;
		createmode = 0600;
	}else{
		g_error("%s.%d: Key type %d is illegal", __FUNCTION__, __LINE__, ktype);
		g_return_val_if_reached(FALSE);
	}
	// If it's a public key, it may exist but not be writable by us...
	if (PUBLICKEY == ktype && g_access(filename, R_OK) == 0) {
		// So, let's check and see if it's what we think it should be...
		if (_cache_curve25519_keypair(key_id)) {
			CryptFramePublicKey*	pub = cryptframe_public_key_by_id(key_id);
			if (pub && memcmp(pub->public_key, key, keysize) == 0) {
				FREE(filename); filename = NULL;
				return TRUE;
			}
		}
	}
	fd = open(filename, O_WRONLY|O_CREAT, createmode);
	if (fd < 0 && (ENOENT == errno)) {
		char*		dirname = _cache_curve25519_key_id_to_dirname(key_id, ktype);
		_cryptcurve25519_make_cryptdir(dirname);
		FREE(dirname);
		fd = open(filename, O_WRONLY|O_CREAT, createmode);
	}
	if (fd < 0) {
		g_warning("%s.%d: cannot create file %s [%s]", __FUNCTION__, __LINE__
		,	filename, g_strerror(errno));
		g_free(filename);
		return FALSE;
	}
	DEBUGMSG4("Saving key to file %s", filename);
	DEBUGCKSUM4("Saved key:", key, keysize);
	rc = write(fd, key, keysize);
	if (rc != keysize) {
		g_warning("%s.%d: cannot write file %s: rc=%d [%s]", __FUNCTION__, __LINE__
		,	filename, rc, g_strerror(errno));
		close(fd);
		g_unlink(filename);
		g_free(filename);
		return FALSE;
	}
	if (close(fd) < 0) {
		g_warning("%s.%d: Close of file %s failed.", __FUNCTION__, __LINE__, filename);
		g_unlink(filename);
		g_free(filename);
		return FALSE;
	}
	chmod(filename, createmode); // Ignore umask...
	DEBUGMSG1("%s.%d: file %s successfully created!", __FUNCTION__, __LINE__, filename);
	g_free(filename);
	return TRUE;
}

/// Generic "new" function to use with cryptframe_set_encryption_method()
WINEXPORT CryptFrame*
cryptcurve25519_new_generic(const char* sender_key_id,		///< sender's key id
			    const char* receiver_key_id,	///< receiver's key id
			    gboolean forsending)		///< TRUE if this is for sending
{
	CryptCurve25519* ret = cryptcurve25519_new(FRAMETYPE_CRYPTCURVE25519, sender_key_id, receiver_key_id, forsending, 0);
	return (ret ? &ret->baseclass: NULL);
}


/// Function just to make setting the encryption method simpler from Python
WINEXPORT void
cryptcurve25519_set_encryption_method(void)
{
        cryptframe_set_encryption_method(cryptcurve25519_new_generic);
}

///@}
