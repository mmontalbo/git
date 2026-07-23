/*
 * Precomputed diff hunks, keyed by diff input.
 *
 * A single store at .git/objects/diff-hunks maps an (old blob, new
 * blob, diff settings) key to the hunk coordinates of diffing the pair.
 * The key determines the diff result, so an entry is valid in any
 * context it recurs in, independent of path. The settings (xdl_opts and
 * context; see struct diff_hunks_settings) are per entry, so one store
 * can hold the hunks computed at several settings, for example a zero
 * context and a nonzero context. Reading is always on; writing is off
 * by default and enabled per run (see diff_hunks_write_enabled), so an
 * ordinary command populates the store only during a warming run the
 * repository owner opts into.
 *
 * File layout:
 *   Header:  "DHPF"(4) + version(1) + hash_version(1) + num_chunks(1) + reserved(1)
 *   Table of contents (chunk-format)
 *   DHIX chunk: sorted entries, each
 *       old_blob_oid, new_blob_oid, xdl_opts(4), context(4), hdat_offset(4)
 *   DHDT chunk: per entry, num_hunks(4) followed by that many 16-byte hunks
 *   Trailing hash checksum
 */
#include "git-compat-util.h"
#include "chunk-format.h"
#include "config.h"
#include "csum-file.h"
#include "diff-hunks.h"
#include "gettext.h"
#include "hash.h"
#include "hashmap.h"
#include "lockfile.h"
#include "odb.h"
#include "path.h"
#include "repo-settings.h"
#include "repository.h"
#include "strbuf.h"
#include "wrapper.h"
#include "xdiff-interface.h"

#define DIFF_HUNKS_SIGNATURE 0x44485046 /* "DHPF" */
/*
 * Bump when the on-disk format changes, or when xdiff's emitted hunk
 * coordinates change for a fixed (blobs, xdl_opts, context): an old store
 * would otherwise serve stale hunks and change command output.
 */
#define DIFF_HUNKS_VERSION 1
#define DIFF_HUNKS_HEADER_SIZE 8

#define DIFF_HUNKS_CHUNKID_INDEX 0x44484958 /* "DHIX" */
#define DIFF_HUNKS_CHUNKID_DATA 0x44484454 /* "DHDT" */

/* Each hunk is 16 bytes on disk: old_start(4) old_count(4) new_start(4) new_count(4) */
#define DIFF_HUNKS_HUNK_SIZE (4 * sizeof(uint32_t))

/*
 * Result of a store lookup: num_hunks records encoded in the store's mmap,
 * valid until the store is freed. Read them with nth_precomputed_hunk().
 */
struct precomputed_entry {
	uint32_t num_hunks;
	const unsigned char *hunk_data;
};

/* Decode a single hunk from the raw on-disk format. */
static inline void decode_precomputed_hunk(const unsigned char *data,
					   struct precomputed_hunk *h)
{
	h->old_start = get_be32(data);
	h->old_count = get_be32(data + 4);
	h->new_start = get_be32(data + 8);
	h->new_count = get_be32(data + 12);
}

/* Decode the nth hunk of a lookup result into *h. */
static inline void nth_precomputed_hunk(const struct precomputed_entry *e,
					uint32_t n, struct precomputed_hunk *h)
{
	decode_precomputed_hunk(e->hunk_data + (size_t)n * DIFF_HUNKS_HUNK_SIZE, h);
}

/* Byte length of the (old_oid, new_oid, xdl_opts, context) lookup key. */
static size_t store_index_key_size(const struct git_hash_algo *algo)
{
	return 2 * algo->rawsz + 2 * sizeof(uint32_t);
}

/* Index entry: the lookup key followed by the 4-byte offset into DHDT. */
static size_t store_index_entry_size(const struct git_hash_algo *algo)
{
	return store_index_key_size(algo) + sizeof(uint32_t);
}

/*
 * The smallest a valid store file can be: the header, a table of contents
 * with one entry per chunk plus a terminating entry, and the trailing
 * checksum.
 */
static size_t store_min_size(const struct git_hash_algo *algo,
			     uint8_t num_chunks)
{
	size_t toc_size = (num_chunks + 1) * CHUNK_TOC_ENTRY_SIZE;

	return DIFF_HUNKS_HEADER_SIZE + toc_size + algo->rawsz;
}

/*
 * Decode an index entry's key into pointers to the two oids and the two
 * settings values (on-disk: old_oid, new_oid, then xdl_opts and context
 * as big-endian uint32s).
 */
static void decode_store_index_key(const unsigned char *entry, unsigned int rawsz,
			     const unsigned char **old_hash,
			     const unsigned char **new_hash,
			     uint32_t *xdl_opts, uint32_t *context)
{
	*old_hash = entry;
	*new_hash = entry + rawsz;
	*xdl_opts = get_be32(entry + 2 * rawsz);
	*context = get_be32(entry + 2 * rawsz + sizeof(uint32_t));
}

/* The DHDT offset stored in an index entry, in the field after its key. */
static uint32_t index_entry_hdat_offset(const unsigned char *entry, size_t keysz)
{
	return get_be32(entry + keysz);
}

static char *store_path_ext(struct repository *r, const char *ext)
{
	struct strbuf sb = STRBUF_INIT;

	strbuf_addf(&sb, "%s/diff-hunks%s", repo_get_object_directory(r), ext);
	return strbuf_detach(&sb, NULL);
}

static char *diff_hunks_store_path(struct repository *r)
{
	return store_path_ext(r, "");
}

/*
 * The store is two tiers: a large consolidated base and a small overlay
 * of the pairs recorded since the last fold. A warm appends to the
 * overlay; "git diff-hunks compact" folds the overlay into the base.
 * Both files use the same on-disk format.
 */
static char *diff_hunks_overlay_path(struct repository *r)
{
	return store_path_ext(r, "-overlay");
}

struct diff_hunks_store {
	const unsigned char *data;
	size_t data_len;
	const struct git_hash_algo *hash_algo;
	const unsigned char *index;
	uint32_t num_entries;
	const unsigned char *hdat;
	size_t hdat_size;
	struct diff_hunks_store *base;	/* older tier, or NULL */
};

static void free_store(struct diff_hunks_store *s)
{
	while (s) {
		struct diff_hunks_store *base = s->base;
		if (s->data)
			munmap((void *)s->data, s->data_len);
		free(s);
		s = base;
	}
}

/*
 * Open, mmap, and parse the store at fname. Returns the parsed store
 * or NULL on any error. The diff output is unaffected either way;
 * corruption is reported by verify, not treated as fatal here.
 */
static struct diff_hunks_store *load_store_at(
		const struct git_hash_algo *repo_algo, const char *fname)
{
	struct diff_hunks_store *s;
	struct chunkfile *cf;
	int fd;
	struct stat st;
	void *data;
	const unsigned char *p;
	uint8_t num_chunks;
	size_t index_size, entry_size, data_len;

	fd = git_open(fname);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) || st.st_size < DIFF_HUNKS_HEADER_SIZE) {
		close(fd);
		return NULL;
	}
	data_len = xsize_t(st.st_size);
	data = xmmap(NULL, data_len, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	p = data;

	num_chunks = p[6];

	/*
	 * Reject a file that is not a readable store: wrong signature,
	 * version, or object hash, or too small to hold the table of
	 * contents that read_table_of_contents() walks (it dereferences
	 * each entry before range-checking its offset).
	 */
	if (get_be32(p) != DIFF_HUNKS_SIGNATURE ||
	    p[4] != DIFF_HUNKS_VERSION ||
	    p[5] != oid_version(repo_algo) ||
	    data_len < store_min_size(repo_algo, num_chunks)) {
		munmap(data, data_len);
		return NULL;
	}

	/*
	 * The trailing checksum is not verified here: the writer fsyncs
	 * and commits atomically, so a committed file is intact, and
	 * every record is bounds-checked at read (see precomputed_entry_at).
	 * The checksum is checked separately, by diff_hunks_verify().
	 */

	CALLOC_ARRAY(s, 1);
	s->data = data;
	s->data_len = data_len;
	s->hash_algo = repo_algo;

	cf = init_chunkfile(NULL);
	if (read_table_of_contents(cf, p, data_len,
				   DIFF_HUNKS_HEADER_SIZE, num_chunks, 1) ||
	    pair_chunk(cf, DIFF_HUNKS_CHUNKID_INDEX, &s->index, &index_size) ||
	    pair_chunk(cf, DIFF_HUNKS_CHUNKID_DATA, &s->hdat, &s->hdat_size)) {
		free_chunkfile(cf);
		goto corrupt;
	}
	free_chunkfile(cf);

	entry_size = store_index_entry_size(s->hash_algo);
	if (index_size % entry_size)
		goto corrupt;
	s->num_entries = index_size / entry_size;
	return s;

corrupt:
	free_store(s);
	return NULL;
}

static struct diff_hunks_store *diff_hunks_store_load(struct repository *r)
{
	struct diff_hunks_store *base, *overlay;
	char *fname;

	prepare_repo_settings(r);
	if (!r->settings.core_diff_hunks)
		return NULL;

	fname = diff_hunks_store_path(r);
	base = load_store_at(r->hash_algo, fname);
	free(fname);

	fname = diff_hunks_overlay_path(r);
	overlay = load_store_at(r->hash_algo, fname);
	free(fname);

	/* Newest tier first; a lookup walks ->base on a miss. */
	if (overlay) {
		overlay->base = base;
		return overlay;
	}
	return base;
}

struct diff_hunks_store *repo_diff_hunks_store(struct repository *r)
{
	if (!r->objects)
		return NULL;
	if (r->objects->diff_hunks_store_attempted)
		return r->objects->diff_hunks_store;
	r->objects->diff_hunks_store_attempted = 1;
	r->objects->diff_hunks_store = diff_hunks_store_load(r);
	return r->objects->diff_hunks_store;
}

void close_diff_hunks_store(struct object_database *o)
{
	if (!o->diff_hunks_store)
		return;
	free_store(o->diff_hunks_store);
	o->diff_hunks_store = NULL;
}

/*
 * Fill *out with the hunk record at offset in the data chunk, and return
 * 1 if the record is in bounds, 0 otherwise. The read path does not
 * re-verify the checksum, and a valid checksum would not bound the count
 * anyway, so a read must call this and use *out only when it returns
 * non-zero.
 *
 * A record is a be32 hunk count followed by that many DIFF_HUNKS_HUNK_SIZE
 * hunks. "remaining" tracks the bytes from offset to the end of the data
 * chunk: it must hold the count, and after the count is consumed it must
 * hold every hunk. The bounds are written as subtraction and division
 * (never addition or multiplication) so a crafted offset or count cannot
 * overflow them.
 */
static int precomputed_entry_at(const struct diff_hunks_store *s,
				uint32_t offset, struct precomputed_entry *out)
{
	size_t remaining;
	uint32_t num_hunks;

	if (offset >= s->hdat_size)
		return 0;
	remaining = s->hdat_size - offset;
	if (remaining < sizeof(uint32_t))
		return 0;

	num_hunks = get_be32(s->hdat + offset);
	remaining -= sizeof(uint32_t);
	if (num_hunks > remaining / DIFF_HUNKS_HUNK_SIZE)
		return 0;

	out->num_hunks = num_hunks;
	out->hunk_data = s->hdat + offset + sizeof(uint32_t);
	return 1;
}

struct lookup_key {
	const struct object_id *old_oid;
	const struct object_id *new_oid;
	struct diff_hunks_settings settings;
	unsigned int rawsz;
};

/*
 * The store's total order over (old_oid, new_oid, xdl_opts, context),
 * defined once so the write-side sort (writer_entry_cmp) and the
 * read-side search (store_bsearch_cmp) order the keys identically.
 */
static int cmp_store_index_key(const unsigned char *old_a, const unsigned char *new_a,
			 uint32_t opts_a, uint32_t ctx_a,
			 const unsigned char *old_b, const unsigned char *new_b,
			 uint32_t opts_b, uint32_t ctx_b, unsigned int rawsz)
{
	int cmp = memcmp(old_a, old_b, rawsz);
	if (!cmp)
		cmp = memcmp(new_a, new_b, rawsz);
	if (!cmp)
		cmp = (opts_a > opts_b) - (opts_a < opts_b);
	if (!cmp)
		cmp = (ctx_a > ctx_b) - (ctx_a < ctx_b);
	return cmp;
}

static int store_bsearch_cmp(const void *key, const void *entry_ptr)
{
	const struct lookup_key *k = key;
	const unsigned char *old_hash, *new_hash;
	uint32_t xdl_opts, context;

	decode_store_index_key(entry_ptr, k->rawsz, &old_hash, &new_hash,
			 &xdl_opts, &context);
	return cmp_store_index_key(k->old_oid->hash, k->new_oid->hash,
			     (uint32_t)k->settings.xdl_opts,
			     (uint32_t)k->settings.context,
			     old_hash, new_hash, xdl_opts, context, k->rawsz);
}

static int store_get_one(struct diff_hunks_store *s, const struct lookup_key *key,
			 struct precomputed_entry *out)
{
	size_t entry_size = store_index_entry_size(s->hash_algo);
	const unsigned char *found;

	found = bsearch(key, s->index, s->num_entries, entry_size,
			store_bsearch_cmp);
	if (!found)
		return 0;
	return precomputed_entry_at(s,
				    index_entry_hdat_offset(found, store_index_key_size(s->hash_algo)),
				    out);
}

static int diff_hunks_store_get(struct diff_hunks_store *s,
			 const struct object_id *old_oid,
			 const struct object_id *new_oid,
			 const struct diff_hunks_settings *ds,
			 struct precomputed_entry *out)
{
	struct lookup_key key;

	if (!s)
		return 0;
	/* The null OID names no blob and cannot key an entry. */
	if (is_null_oid(old_oid) || is_null_oid(new_oid))
		return 0;

	key.old_oid = old_oid;
	key.new_oid = new_oid;
	key.settings = *ds;
	key.rawsz = s->hash_algo->rawsz;

	/* Content-keyed, so any tier's hit is identical; take the first. */
	for (; s; s = s->base)
		if (store_get_one(s, &key, out))
			return 1;
	return 0;
}

int diff_hunks_store_sum(struct diff_hunks_store *s,
			 const struct object_id *old_oid,
			 const struct object_id *new_oid,
			 const struct diff_hunks_settings *ds,
			 uintmax_t *added, uintmax_t *deleted)
{
	struct precomputed_entry e;
	uint32_t i;

	if (!diff_hunks_store_get(s, old_oid, new_oid, ds, &e))
		return 0;
	for (i = 0; i < e.num_hunks; i++) {
		struct precomputed_hunk h;
		nth_precomputed_hunk(&e, i, &h);
		*added += h.new_count;
		*deleted += h.old_count;
	}
	return 1;
}

/*
 * A recorded hunk sequence is replayable if every coordinate fits in an int
 * (a consumer may truncate to int) and the running old-vs-new offset stays
 * consistent, as a well-formed diff's hunks always are. Coordinates decode
 * from be32 into long, which is 32-bit on some platforms, so a crafted value
 * can decode negative: bound each field to [0, INT32_MAX]. A store that
 * fails this is treated as a miss, so the caller recomputes.
 */
static int replayable_hunks(const struct precomputed_entry *e)
{
	int64_t offset = 0;
	uint32_t i;

	for (i = 0; i < e->num_hunks; i++) {
		struct precomputed_hunk h;
		nth_precomputed_hunk(e, i, &h);
		if (h.old_start < 0 || h.old_count < 0 ||
		    h.new_start < 0 || h.new_count < 0 ||
		    h.old_start > INT32_MAX || h.old_count > INT32_MAX ||
		    h.new_start > INT32_MAX || h.new_count > INT32_MAX ||
		    h.old_start - h.new_start != offset)
			return 0;
		offset = (int64_t)h.old_start + h.old_count -
			 ((int64_t)h.new_start + h.new_count);
	}
	return 1;
}

int diff_hunks_emit(struct diff_hunks_store *store,
		    const struct object_id *old_oid,
		    const struct object_id *new_oid,
		    const struct diff_hunks_settings *ds,
		    diff_hunks_fill_fn fill, void *fill_data,
		    xdl_emit_hunk_consume_func_t hunk_func, void *cb_data)
{
	mmfile_t mf_old, mf_new;
	xpparam_t xpp = { 0 };
	xdemitconf_t xecfg = { 0 };
	xdemitcb_t ecb = { NULL };

	if (store) {
		struct precomputed_entry e;
		if (diff_hunks_store_get(store, old_oid, new_oid, ds, &e) &&
		    replayable_hunks(&e)) {
			uint32_t i;
			for (i = 0; i < e.num_hunks; i++) {
				struct precomputed_hunk h;
				nth_precomputed_hunk(&e, i, &h);
				hunk_func(h.old_start, h.old_count,
					  h.new_start, h.new_count, cb_data);
			}
			return 1;
		}
	}

	if (fill(fill_data, &mf_old, &mf_new))
		return -1;
	xpp.flags = ds->xdl_opts;
	xecfg.ctxlen = xecfg.interhunkctxlen = ds->context;
	xecfg.hunk_func = hunk_func;
	ecb.priv = cb_data;
	return xdi_diff(&mf_old, &mf_new, &xpp, &xecfg, &ecb) ? -1 : 0;
}

/* Validate one store file. Returns 0 if valid or absent, -1 if corrupt. */
static int verify_store_at(struct repository *r, const char *fname)
{
	struct diff_hunks_store *s;
	size_t entry_size;
	uint32_t i;
	int ret = 0;

	if (access(fname, F_OK))
		return 0; /* absent is valid */
	s = load_store_at(r->hash_algo, fname);
	if (!s)
		return error(_("diff-hunks store failed to load (corrupt "
			       "header or hash mismatch): %s"), fname);
	if (!hashfile_checksum_valid(r->hash_algo, s->data, s->data_len)) {
		error(_("diff-hunks store has incorrect checksum and is "
			"likely corrupt: %s"), fname);
		free_store(s);
		return -1;
	}

	entry_size = store_index_entry_size(s->hash_algo);
	for (i = 0; i < s->num_entries; i++) {
		const unsigned char *ep = s->index + st_mult(entry_size, i);
		size_t keysz = store_index_key_size(s->hash_algo);
		uint32_t offset = index_entry_hdat_offset(ep, keysz);
		struct precomputed_entry pe;

		/*
		 * Keyed by (old_oid, new_oid, xdl_opts, context), increasing.
		 * memcmp matches cmp_store_index_key's integer comparison of
		 * xdl_opts and context because both are non-negative, so their
		 * big-endian bytes order the same as their values.
		 */
		if (i > 0 && memcmp(ep - entry_size, ep, keysz) >= 0) {
			error(_("diff-hunks entry %u not in sorted order"), i);
			ret = -1;
		}
		if (!precomputed_entry_at(s, offset, &pe)) {
			error(_("diff-hunks entry %u has out-of-bounds hunk "
				"data"), i);
			ret = -1;
		}
	}

	free_store(s);
	return ret;
}

int diff_hunks_verify(struct repository *r)
{
	char *base = diff_hunks_store_path(r);
	char *overlay = diff_hunks_overlay_path(r);
	int ret = 0;

	if (verify_store_at(r, base))
		ret = -1;
	if (verify_store_at(r, overlay))
		ret = -1;
	free(base);
	free(overlay);
	return ret;
}

int diff_hunks_clear(struct repository *r)
{
	char *base = diff_hunks_store_path(r);
	char *overlay = diff_hunks_overlay_path(r);
	int ret = 0;

	if (unlink(base) && errno != ENOENT)
		ret = error_errno(_("unable to remove %s"), base);
	if (unlink(overlay) && errno != ENOENT)
		ret = error_errno(_("unable to remove %s"), overlay);
	free(base);
	free(overlay);
	return ret;
}

struct writer_entry {
	struct object_id old_oid;
	struct object_id new_oid;
	struct diff_hunks_settings settings;
	uint32_t hdat_offset;
};

struct diff_hunks_writer {
	struct repository *r;
	struct writer_entry *entries;
	size_t nr, alloc;
	size_t seed_nr;		/* nr after seeding; finish skips a no-op flush */
	struct strbuf hdat;
	struct hashmap dedup;	/* hunk block content -> offset in hdat */
};

/* A record of one distinct hunk block already present in hdat. */
struct dedup_entry {
	struct hashmap_entry ent;
	uint32_t offset;
	uint32_t len;
};

static int dedup_cmp(const void *cmp_data,
		     const struct hashmap_entry *a,
		     const struct hashmap_entry *b,
		     const void *keydata UNUSED)
{
	const struct diff_hunks_writer *writer = cmp_data;
	const struct dedup_entry *ea = container_of(a, const struct dedup_entry, ent);
	const struct dedup_entry *eb = container_of(b, const struct dedup_entry, ent);

	if (ea->len != eb->len)
		return 1;
	return memcmp(writer->hdat.buf + ea->offset,
		      writer->hdat.buf + eb->offset, ea->len);
}

static struct diff_hunks_writer *diff_hunks_writer_new(struct repository *r)
{
	struct diff_hunks_writer *w;

	CALLOC_ARRAY(w, 1);
	w->r = r;
	strbuf_init(&w->hdat, 0);
	hashmap_init(&w->dedup, dedup_cmp, w, 0);
	return w;
}

static void strbuf_put_be32(struct strbuf *sb, uint32_t val)
{
	unsigned char buf[4];
	put_be32(buf, val);
	strbuf_add(sb, buf, 4);
}

/*
 * The hunk block just appended at `start` is deduplicated: if an
 * identical block is already in hdat, this copy is dropped and the
 * earlier offset returned; otherwise it is kept and remembered. Distinct
 * keys that diff to the same hunks then share one block, so keying the
 * same hunks under several settings costs one block, not one per setting.
 */
static uint32_t intern_block(struct diff_hunks_writer *w, size_t start)
{
	size_t len = w->hdat.len - start;
	struct dedup_entry key, *found, *added;

	hashmap_entry_init(&key.ent, memhash(w->hdat.buf + start, len));
	key.offset = (uint32_t)start;
	key.len = (uint32_t)len;

	found = hashmap_get_entry(&w->dedup, &key, ent, NULL);
	if (found) {
		strbuf_setlen(&w->hdat, start);
		return found->offset;
	}

	added = xmalloc(sizeof(*added));
	hashmap_entry_init(&added->ent, key.ent.hash);
	added->offset = key.offset;
	added->len = key.len;
	hashmap_add(&w->dedup, &added->ent);
	return key.offset;
}

void diff_hunks_writer_add(struct diff_hunks_writer *w,
			    const struct object_id *old_oid,
			    const struct object_id *new_oid,
			    const struct diff_hunks_settings *ds,
			    const struct precomputed_hunk *hunks,
			    size_t nr_hunks)
{
	struct writer_entry *e;
	size_t i, block_start;

	/*
	 * The block appended for this entry is sizeof(uint32_t) +
	 * nr_hunks * DIFF_HUNKS_HUNK_SIZE bytes. Bound nr_hunks so that
	 * length fits the uint32_t the dedup index records (and so the
	 * count itself fits the uint32_t written to the store).
	 */
	if (!nr_hunks ||
	    nr_hunks > (UINT32_MAX - sizeof(uint32_t)) / DIFF_HUNKS_HUNK_SIZE ||
	    is_null_oid(old_oid) || is_null_oid(new_oid))
		return;
	if (w->hdat.len > UINT32_MAX)
		return;
	/*
	 * Coordinates are stored as 32-bit values; a result that cannot
	 * round-trip is dropped rather than silently truncated.
	 */
	for (i = 0; i < nr_hunks; i++)
		if ((uintmax_t)hunks[i].old_start > (uintmax_t)INT32_MAX ||
		    (uintmax_t)hunks[i].old_count > (uintmax_t)INT32_MAX ||
		    (uintmax_t)hunks[i].new_start > (uintmax_t)INT32_MAX ||
		    (uintmax_t)hunks[i].new_count > (uintmax_t)INT32_MAX)
			return;

	ALLOC_GROW(w->entries, w->nr + 1, w->alloc);
	e = &w->entries[w->nr++];
	oidcpy(&e->old_oid, old_oid);
	oidcpy(&e->new_oid, new_oid);
	e->settings = *ds;

	block_start = w->hdat.len;
	strbuf_put_be32(&w->hdat, (uint32_t)nr_hunks);
	for (i = 0; i < nr_hunks; i++) {
		strbuf_put_be32(&w->hdat, hunks[i].old_start);
		strbuf_put_be32(&w->hdat, hunks[i].old_count);
		strbuf_put_be32(&w->hdat, hunks[i].new_start);
		strbuf_put_be32(&w->hdat, hunks[i].new_count);
	}
	e->hdat_offset = intern_block(w, block_start);
}

/*
 * Seed the writer with fname's entries so a rewrite preserves them. Returns
 * 0 when the file is absent or seeded, and -1 when it is present but fails
 * its checksum. A rewrite re-checksums, so a corrupt source must not be
 * carried forward: that would launder the corruption into a checksum-valid
 * file that verify can no longer catch. This path already reads the whole
 * file, so verify it here (the reader stays trust-at-read and fast) and
 * discard it on mismatch, leaving the caller to react: a warm proceeds
 * without it, a fold refuses to consolidate a shrunken store.
 */
static int diff_hunks_writer_seed(struct diff_hunks_writer *w, const char *fname)
{
	struct diff_hunks_store *s = load_store_at(w->r->hash_algo, fname);
	unsigned int rawsz;
	size_t entry_size, keysz;
	struct precomputed_hunk *hunks = NULL;
	size_t hunks_alloc = 0;
	uint32_t i;

	if (!s)
		return 0;
	if (!hashfile_checksum_valid(w->r->hash_algo, s->data, s->data_len)) {
		warning(_("diff-hunks store %s failed its checksum; "
			  "discarding it"), fname);
		free_store(s);
		return -1;
	}
	rawsz = s->hash_algo->rawsz;
	entry_size = store_index_entry_size(s->hash_algo);
	keysz = store_index_key_size(s->hash_algo);

	for (i = 0; i < s->num_entries; i++) {
		const unsigned char *ep = s->index + st_mult(entry_size, i);
		const unsigned char *old_hash, *new_hash;
		struct object_id old_oid, new_oid;
		struct diff_hunks_settings ds;
		uint32_t xdl_opts, context, j;
		struct precomputed_entry pe;

		decode_store_index_key(ep, rawsz, &old_hash, &new_hash,
				 &xdl_opts, &context);
		oidread(&old_oid, old_hash, s->hash_algo);
		oidread(&new_oid, new_hash, s->hash_algo);
		ds.xdl_opts = (int)xdl_opts;
		ds.context = (int)context;
		if (!precomputed_entry_at(s, index_entry_hdat_offset(ep, keysz), &pe))
			continue;
		ALLOC_GROW(hunks, pe.num_hunks, hunks_alloc);
		for (j = 0; j < pe.num_hunks; j++)
			nth_precomputed_hunk(&pe, j, &hunks[j]);
		diff_hunks_writer_add(w, &old_oid, &new_oid, &ds,
				       hunks, pe.num_hunks);
	}
	free(hunks);
	free_store(s);
	return 0;
}

/*
 * Writing is off by default. It is enabled per invocation by the
 * GIT_DIFF_HUNKS_WRITE environment variable, or persistently by the
 * diffHunks.write config, with the environment variable winning when
 * set. Only a warming run (a diff or log the repository owner chooses
 * to run with writing on) enables it, so ordinary reads never mutate
 * the store.
 */
static int diff_hunks_write_enabled(struct repository *r)
{
	const char *env = getenv("GIT_DIFF_HUNKS_WRITE");
	int val;

	if (env) {
		/*
		 * This is a warming opt-in, so an unparseable value must not
		 * abort an ordinary read command: treat it as disabled.
		 */
		val = git_parse_maybe_bool(env);
		return val < 0 ? 0 : val;
	}
	if (!repo_config_get_bool(r, "diffhunks.write", &val))
		return val;
	return 0;
}

struct diff_hunks_writer *diff_hunks_writer_maybe_new(struct repository *r)
{
	struct diff_hunks_writer *w;
	char *fname;

	if (!diff_hunks_write_enabled(r))
		return NULL;
	/*
	 * A warm appends to the overlay tier: seed from the overlay (small)
	 * so a flush merges with it rather than replacing it, and leave the
	 * base untouched. "git diff-hunks compact" folds overlay into base.
	 */
	w = diff_hunks_writer_new(r);
	fname = diff_hunks_overlay_path(r);
	diff_hunks_writer_seed(w, fname);
	free(fname);
	w->seed_nr = w->nr;
	return w;
}

static int writer_entry_cmp(const void *va, const void *vb, void *ctx)
{
	const struct writer_entry *a = va, *b = vb;
	unsigned int rawsz = *(const unsigned int *)ctx;
	return cmp_store_index_key(a->old_oid.hash, a->new_oid.hash,
			     (uint32_t)a->settings.xdl_opts,
			     (uint32_t)a->settings.context,
			     b->old_oid.hash, b->new_oid.hash,
			     (uint32_t)b->settings.xdl_opts,
			     (uint32_t)b->settings.context,
			     rawsz);
}

struct write_ctx {
	struct diff_hunks_writer *w;
	unsigned int rawsz;
};

static int write_index_chunk(struct hashfile *f, void *data)
{
	struct write_ctx *ctx = data;
	size_t i;

	for (i = 0; i < ctx->w->nr; i++) {
		hashwrite(f, ctx->w->entries[i].old_oid.hash, ctx->rawsz);
		hashwrite(f, ctx->w->entries[i].new_oid.hash, ctx->rawsz);
		hashwrite_be32(f, ctx->w->entries[i].settings.xdl_opts);
		hashwrite_be32(f, ctx->w->entries[i].settings.context);
		hashwrite_be32(f, ctx->w->entries[i].hdat_offset);
	}
	return 0;
}

static int write_data_chunk(struct hashfile *f, void *data)
{
	struct write_ctx *ctx = data;
	hashwrite(f, ctx->w->hdat.buf, ctx->w->hdat.len);
	return 0;
}

/* Sort, dedup, and write the accumulated entries to the file at fname. */
static int diff_hunks_writer_flush(struct diff_hunks_writer *w, char *fname)
{
	struct lock_file lk = LOCK_INIT;
	struct hashfile *f;
	struct chunkfile *cf;
	unsigned int rawsz = w->r->hash_algo->rawsz;
	struct write_ctx ctx = { w, rawsz };
	size_t entry_size;

	QSORT_S(w->entries, w->nr, writer_entry_cmp, &rawsz);

	/*
	 * The same blob pair recurs across history (reverts, cherry-
	 * picks); identical keys carry identical hunks, so keep one of
	 * each. The index must stay duplicate-free for binary search.
	 */
	if (w->nr > 1) {
		size_t kept = 1, i;
		for (i = 1; i < w->nr; i++)
			if (writer_entry_cmp(&w->entries[kept - 1],
					      &w->entries[i], &rawsz))
				w->entries[kept++] = w->entries[i];
		w->nr = kept;
	}

	if (safe_create_leading_directories(w->r, fname)) {
		error(_("unable to create directory for %s"), fname);
		return -1;
	}
	if (hold_lock_file_for_update(&lk, fname, 0) < 0) {
		error_errno(_("unable to lock %s"), fname);
		return -1;
	}
	adjust_shared_perm(w->r, get_lock_file_path(&lk));
	f = hashfd(w->r->hash_algo, get_lock_file_fd(&lk),
		   get_lock_file_path(&lk));

	entry_size = store_index_entry_size(w->r->hash_algo);
	cf = init_chunkfile(f);
	add_chunk(cf, DIFF_HUNKS_CHUNKID_INDEX, w->nr * entry_size,
		  write_index_chunk);
	add_chunk(cf, DIFF_HUNKS_CHUNKID_DATA, w->hdat.len, write_data_chunk);

	hashwrite_be32(f, DIFF_HUNKS_SIGNATURE);
	hashwrite_u8(f, DIFF_HUNKS_VERSION);
	hashwrite_u8(f, oid_version(w->r->hash_algo));
	hashwrite_u8(f, get_num_chunks(cf));
	hashwrite_u8(f, 0); /* reserved */

	write_chunkfile(cf, &ctx);
	free_chunkfile(cf);

	/*
	 * fsync per the user's configuration (like commit-graph and the
	 * multi-pack-index), then commit atomically. Readers trust the
	 * committed file rather than re-checksumming it; diff_hunks_verify()
	 * checks the checksum separately.
	 */
	finalize_hashfile(f, NULL, FSYNC_COMPONENT_DIFF_HUNKS,
			  CSUM_HASH_IN_STREAM);
	if (commit_lock_file(&lk)) {
		error_errno(_("unable to write %s"), fname);
		return -1;
	}
	return 0;
}

static void diff_hunks_writer_free(struct diff_hunks_writer *w)
{
	if (!w)
		return;
	hashmap_clear_and_free(&w->dedup, struct dedup_entry, ent);
	free(w->entries);
	strbuf_release(&w->hdat);
	free(w);
}

void diff_hunks_writer_finish(struct diff_hunks_writer *w)
{
	if (!w)
		return;
	/* Skip the flush when the warm recorded nothing beyond its seed. */
	if (w->nr != w->seed_nr) {
		char *fname = diff_hunks_overlay_path(w->r);
		diff_hunks_writer_flush(w, fname);
		free(fname);
	}
	diff_hunks_writer_free(w);
}

int diff_hunks_compact(struct repository *r)
{
	char *base = diff_hunks_store_path(r);
	char *overlay = diff_hunks_overlay_path(r);
	struct diff_hunks_writer *w;
	int ret = 0;

	/* Nothing to fold if there is no overlay; leave the base alone. */
	if (access(overlay, F_OK)) {
		free(base);
		free(overlay);
		return 0;
	}

	/*
	 * Merge the base and overlay into a fresh base, then drop the overlay.
	 * If either tier fails its checksum, seeding discards it; refuse to
	 * consolidate rather than rewrite a base that omits the discarded
	 * entries and then delete the overlay.
	 */
	w = diff_hunks_writer_new(r);
	if (diff_hunks_writer_seed(w, base) < 0 ||
	    diff_hunks_writer_seed(w, overlay) < 0) {
		diff_hunks_writer_free(w);
		ret = error(_("diff-hunks store is corrupt; run "
			      "'git diff-hunks clear' and warm it again"));
	} else {
		ret = diff_hunks_writer_flush(w, base);
		diff_hunks_writer_free(w);
		if (!ret && unlink(overlay) && errno != ENOENT)
			ret = error_errno(_("unable to remove %s"), overlay);
	}

	free(base);
	free(overlay);
	return ret;
}
