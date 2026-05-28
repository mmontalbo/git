/*
 * Precomputed diff hunks for blame and diffstat acceleration.
 *
 * Stores hunk positions (old_start, old_count, new_start, new_count)
 * per (commit, parent) in per-path cache files using chunk-format.
 * Consumers read these instead of decompressing blobs and running xdiff.
 *
 * Per-path file layout:
 *   Header:  "DHPF"(4) + version(1) + hash_version(1) + num_chunks(1) + pad(1)
 *   Table of contents (chunk-format)
 *   DHMF chunk: xdl_opts(4)
 *   DHIX chunk: sorted (commit_oid, parent_oid, hdat_offset) entries
 *   DHDT chunk: variable-length hunk data
 *   Trailing hash checksum
 */
#include "git-compat-util.h"
#include "chunk-format.h"
#include "diff-hunks.h"
#include "gettext.h"
#include "hash.h"
#include "odb.h"
#include "odb/source.h"
#include "repository.h"
#include "strbuf.h"
#include "strmap.h"
#include "wrapper.h"

#define DIFF_HUNKS_SIGNATURE 0x44485046 /* "DHPF" */
#define DIFF_HUNKS_VERSION 1
#define DIFF_HUNKS_HEADER_SIZE 8

/* Chunk IDs */
#define DHPF_CHUNKID_META 0x44484d46 /* "DHMF" */
#define DHPF_CHUNKID_INDEX 0x44484958 /* "DHIX" */
#define DHPF_CHUNKID_DATA 0x44484454 /* "DHDT" */

/* --- Internal types --- */

struct diff_hunks_path_file {
	const unsigned char *data;
	size_t data_len;
	const struct git_hash_algo *hash_algo;
	uint32_t stored_xdl_opts;
	const unsigned char *index;
	uint32_t num_entries;
	const unsigned char *hdat;
	size_t hdat_size;
};

/* --- Per-path reader --- */

static size_t path_index_entry_size(const struct git_hash_algo *algo)
{
	return algo->rawsz + algo->rawsz + 4;
}

/*
 * Per-path cache files are stored under .git/objects/diff-hunks/ in
 * 256 shard directories (like loose objects) to avoid filesystem
 * performance problems with many files in a single directory.
 * The shard is the low byte of strhash(path).
 */
static char *get_path_hunks_filename(struct repository *r, const char *path)
{
	struct strbuf fname = STRBUF_INIT;
	unsigned int shard = strhash(path) & 0xff;

	strbuf_addf(&fname, "%s/diff-hunks/%02x/",
		    repo_get_object_directory(r), shard);
	strbuf_add_percentencode(&fname, path, STRBUF_ENCODE_SLASH);

	return strbuf_detach(&fname, NULL);
}

static int read_meta_chunk(const unsigned char *chunk_start,
			   size_t chunk_size, void *data)
{
	struct diff_hunks_path_file *pf = data;

	if (chunk_size < 4)
		return error(_("diff-hunks meta chunk too small"));

	pf->stored_xdl_opts = get_be32(chunk_start);
	return 0;
}

static void free_path_hunks(struct diff_hunks_path_file *pf);

static struct diff_hunks_path_file *load_path_hunks(struct repository *r,
					     const char *path)
{
	struct diff_hunks_path_file *pf;
	struct chunkfile *cf = NULL;
	char *fname;
	int fd;
	struct stat st;
	void *data;
	const unsigned char *p;
	uint32_t signature;
	uint8_t version, hash_ver, num_chunks;
	size_t index_size, entry_size;

	fname = get_path_hunks_filename(r, path);
	fd = git_open(fname);
	free(fname);
	if (fd < 0)
		return NULL;

	if (fstat(fd, &st) || st.st_size < DIFF_HUNKS_HEADER_SIZE) {
		close(fd);
		return NULL;
	}

	data = xmmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	p = data;

	signature = get_be32(p);
	if (signature != DIFF_HUNKS_SIGNATURE) {
		munmap(data, st.st_size);
		return NULL;
	}

	version = p[4];
	if (version != DIFF_HUNKS_VERSION) {
		munmap(data, st.st_size);
		return NULL;
	}

	hash_ver = p[5];
	num_chunks = p[6];

	CALLOC_ARRAY(pf, 1);
	pf->data = data;
	pf->data_len = st.st_size;

	if (hash_ver == 1)
		pf->hash_algo = &hash_algos[GIT_HASH_SHA1];
	else if (hash_ver == 2)
		pf->hash_algo = &hash_algos[GIT_HASH_SHA256];
	else {
		free_path_hunks(pf);
		return NULL;
	}

	if (pf->hash_algo != r->hash_algo) {
		free_path_hunks(pf);
		return NULL;
	}

	cf = init_chunkfile(NULL);

	if (read_table_of_contents(cf, p, st.st_size,
				   DIFF_HUNKS_HEADER_SIZE, num_chunks, 1)) {
		free_chunkfile(cf);
		free_path_hunks(pf);
		return NULL;
	}

	if (read_chunk(cf, DHPF_CHUNKID_META, read_meta_chunk, pf)) {
		free_chunkfile(cf);
		free_path_hunks(pf);
		return NULL;
	}

	if (pair_chunk(cf, DHPF_CHUNKID_INDEX,
		       &pf->index, &index_size)) {
		free_chunkfile(cf);
		free_path_hunks(pf);
		return NULL;
	}

	if (pair_chunk(cf, DHPF_CHUNKID_DATA,
		       &pf->hdat, &pf->hdat_size)) {
		free_chunkfile(cf);
		free_path_hunks(pf);
		return NULL;
	}

	entry_size = path_index_entry_size(pf->hash_algo);
	pf->num_entries = index_size / entry_size;

	free_chunkfile(cf);
	return pf;
}

static void free_path_hunks(struct diff_hunks_path_file *pf)
{
	if (!pf)
		return;
	if (pf->data)
		munmap((void *)pf->data, pf->data_len);
	free(pf);
}

/*
 * Look up precomputed hunks for a (commit, parent) pair.
 * The caller must ensure pf is non-NULL and was successfully loaded.
 * Returns 1 if found, 0 on miss.
 */
struct lookup_key {
	const unsigned char *commit_hash;
	const unsigned char *parent_hash;
	unsigned int rawsz;
};

static int path_entry_bsearch_cmp(const void *key, const void *entry_ptr)
{
	const struct lookup_key *k = key;
	const unsigned char *entry = entry_ptr;
	int cmp = memcmp(k->commit_hash, entry, k->rawsz);
	if (!cmp)
		cmp = memcmp(k->parent_hash, entry + k->rawsz, k->rawsz);
	return cmp;
}

static int path_hunks_lookup(const struct diff_hunks_path_file *pf,
		      const struct object_id *commit_oid,
		      const struct object_id *parent_oid,
		      struct precomputed_entry *out)
{
	size_t entry_size;
	unsigned int rawsz;
	struct lookup_key key;
	const unsigned char *found;

	if (!pf || !pf->index || !pf->hdat)
		return 0;

	entry_size = path_index_entry_size(pf->hash_algo);
	rawsz = pf->hash_algo->rawsz;

	key.commit_hash = commit_oid->hash;
	key.parent_hash = parent_oid->hash;
	key.rawsz = rawsz;

	found = bsearch(&key, pf->index, pf->num_entries,
			 entry_size, path_entry_bsearch_cmp);
	if (found) {
		uint32_t offset = get_be32(found + 2 * rawsz);
		const unsigned char *hdata = pf->hdat + offset;
		const unsigned char *end = pf->hdat + pf->hdat_size;
		uint32_t num_hunks;

		if (offset >= pf->hdat_size || hdata + 4 > end)
			return 0;
		num_hunks = get_be32(hdata);
		hdata += 4;
		if (hdata + (size_t)num_hunks * DIFF_HUNKS_ENTRY_SIZE > end)
			return 0;

		out->num_hunks = num_hunks;
		out->hunk_data = hdata;
		return 1;
	}
	return 0;
}

/* --- Consumer cache API --- */

/* Sentinel for "tried to load but no file exists" */
static struct diff_hunks_path_file path_hunks_none;

struct diff_hunks_cache {
	struct repository *r;
	int xdl_opts;
	struct strmap map;
};

struct diff_hunks_cache *diff_hunks_cache_init(struct repository *r,
					       int xdl_opts)
{
	struct diff_hunks_cache *c;

	CALLOC_ARRAY(c, 1);
	c->r = r;
	c->xdl_opts = xdl_opts;
	strmap_init(&c->map);
	return c;
}

void diff_hunks_cache_free(struct diff_hunks_cache *c)
{
	struct hashmap_iter iter;
	struct strmap_entry *e;

	if (!c)
		return;

	strmap_for_each_entry(&c->map, &iter, e) {
		if (e->value && e->value != &path_hunks_none)
			free_path_hunks(e->value);
	}
	strmap_clear(&c->map, 0);
	free(c);
}

int diff_hunks_get(struct diff_hunks_cache *c,
		   const struct object_id *commit_oid,
		   const struct object_id *parent_oid,
		   const char *path,
		   struct precomputed_entry *out)
{
	struct diff_hunks_path_file *pf;
	void *cached;

	if (!c)
		return 0;

	cached = strmap_get(&c->map, path);
	if (cached) {
		pf = (cached == &path_hunks_none) ? NULL : cached;
	} else {
		pf = load_path_hunks(c->r, path);
		strmap_put(&c->map, path,
			   pf ? (void *)pf : (void *)&path_hunks_none);
	}

	if (!pf)
		return 0;

	if ((int)pf->stored_xdl_opts != c->xdl_opts)
		return 0;

	return path_hunks_lookup(pf, commit_oid, parent_oid, out);
}
