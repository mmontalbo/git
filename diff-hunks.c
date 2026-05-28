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
#include "commit.h"
#include "csum-file.h"
#include "diff.h"
#include "diff-hunks.h"
#include "diffcore.h"
#include "gettext.h"
#include "hash.h"
#include "lockfile.h"
#include "object.h"
#include "odb.h"
#include "odb/source.h"
#include "path.h"
#include "progress.h"
#include "repository.h"
#include "revision.h"
#include "strbuf.h"
#include "strmap.h"
#include "tree.h"
#include "wrapper.h"
#include "xdiff-interface.h"

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

/* --- Per-path writer --- */

struct collected_hunk {
	uint32_t old_start;
	uint16_t old_count;
	uint32_t new_start;
	uint16_t new_count;
};

struct hunk_collector {
	struct collected_hunk *hunks;
	size_t nr, alloc;
	int overflow;
};

static int collect_hunks_hunk(long start_a, long count_a,
			      long start_b, long count_b,
			      void *cb_data)
{
	struct hunk_collector *hc = cb_data;
	if (hc->overflow)
		return 0;
	if (start_a < 0 || start_b < 0 ||
	    (unsigned long)start_a > UINT32_MAX ||
	    (unsigned long)start_b > UINT32_MAX ||
	    count_a > UINT16_MAX || count_b > UINT16_MAX) {
		hc->overflow = 1;
		return 0;
	}
	ALLOC_GROW(hc->hunks, hc->nr + 1, hc->alloc);
	hc->hunks[hc->nr].old_start = start_a;
	hc->hunks[hc->nr].old_count = count_a;
	hc->hunks[hc->nr].new_start = start_b;
	hc->hunks[hc->nr].new_count = count_b;
	hc->nr++;
	return 0;
}

static void strbuf_put_be32(struct strbuf *sb, uint32_t val)
{
	unsigned char buf[4];
	put_be32(buf, val);
	strbuf_add(sb, buf, 4);
}

static void strbuf_put_be16(struct strbuf *sb, uint16_t val)
{
	unsigned char buf[2];
	buf[0] = (val >> 8) & 0xff;
	buf[1] = val & 0xff;
	strbuf_add(sb, buf, 2);
}

struct path_entry {
	struct object_id commit_oid;
	struct object_id parent_oid;
	uint32_t hdat_offset;
};

static int path_entry_cmp(const void *va, const void *vb)
{
	const struct path_entry *a = va, *b = vb;
	int cmp = oidcmp(&a->commit_oid, &b->commit_oid);
	if (cmp)
		return cmp;
	return oidcmp(&a->parent_oid, &b->parent_oid);
}

static int ensure_diff_hunks_dir(struct repository *r, const char *path)
{
	struct strbuf dir = STRBUF_INIT;
	unsigned int shard = strhash(path) & 0xff;
	int ret = 0;

	strbuf_addf(&dir, "%s/diff-hunks",
		    repo_get_object_directory(r));
	if (mkdir(dir.buf, 0777) && errno != EEXIST) {
		ret = error_errno(_("unable to create directory %s"), dir.buf);
		goto out;
	}

	strbuf_addf(&dir, "/%02x", shard);
	if (mkdir(dir.buf, 0777) && errno != EEXIST) {
		ret = error_errno(_("unable to create directory %s"), dir.buf);
		goto out;
	}

out:
	strbuf_release(&dir);
	return ret;
}

/* Chunk write callbacks */

struct write_path_hunks_ctx {
	struct repository *r;
	int xdl_opts;
	struct path_entry *entries;
	size_t entries_nr;
	struct strbuf *hdat;
};

static int write_meta_chunk(struct hashfile *f, void *data)
{
	struct write_path_hunks_ctx *ctx = data;
	hashwrite_be32(f, (uint32_t)ctx->xdl_opts);
	return 0;
}

static int write_index_chunk(struct hashfile *f, void *data)
{
	struct write_path_hunks_ctx *ctx = data;
	size_t i;
	unsigned int rawsz = ctx->r->hash_algo->rawsz;

	for (i = 0; i < ctx->entries_nr; i++) {
		hashwrite(f, ctx->entries[i].commit_oid.hash, rawsz);
		hashwrite(f, ctx->entries[i].parent_oid.hash, rawsz);
		hashwrite_be32(f, ctx->entries[i].hdat_offset);
	}
	return 0;
}

static int write_data_chunk(struct hashfile *f, void *data)
{
	struct write_path_hunks_ctx *ctx = data;
	hashwrite(f, ctx->hdat->buf, ctx->hdat->len);
	return 0;
}

int write_path_hunks_file(struct repository *r,
			  const char *path,
			  int xdl_opts)
{
	struct rev_info revs = { 0 };
	struct commit *commit;
	const char *argv[] = { "rev-list", "--all", "--topo-order", "--", path, NULL };
	struct setup_revision_opt rev_opt = { 0 };
	struct lock_file lk = LOCK_INIT;
	struct hashfile *f;
	struct chunkfile *cf;
	struct write_path_hunks_ctx ctx;
	char *fname;

	struct path_entry *entries = NULL;
	size_t entries_nr = 0, entries_alloc = 0;
	struct strbuf hdat = STRBUF_INIT;
	size_t entry_size;

	repo_init_revisions(r, &revs, NULL);
	setup_revisions(ARRAY_SIZE(argv) - 1, argv, &revs, &rev_opt);

	if (prepare_revision_walk(&revs))
		return error(_("revision walk setup failed"));

	while ((commit = get_revision(&revs)) != NULL) {
		struct commit_list *parent;

		if (!commit->parents)
			continue;

		for (parent = commit->parents; parent; parent = parent->next) {
			struct diff_options diffopt;
			struct hunk_collector hc = { 0 };
			int j, err;

			repo_diff_setup(r, &diffopt);
			diffopt.flags.recursive = 1;
			diffopt.detect_rename = 0;
			diffopt.output_format = DIFF_FORMAT_NO_OUTPUT;
			diffopt.no_free = 1;
			diff_setup_done(&diffopt);

			diff_tree_oid(&parent->item->object.oid,
				      &commit->object.oid, "", &diffopt);
			diffcore_std(&diffopt);

			for (j = 0; j < diff_queued_diff.nr; j++) {
				struct diff_filepair *p = diff_queued_diff.queue[j];
				xpparam_t xpp = { 0 };
				xdemitconf_t xecfg = { 0 };
				xdemitcb_t ecb = { 0 };
				mmfile_t mf1, mf2;

				if (!DIFF_FILE_VALID(p->one) || !DIFF_FILE_VALID(p->two))
					continue;
				if (!S_ISREG(p->one->mode) || !S_ISREG(p->two->mode))
					continue;
				if (strcmp(p->two->path, path))
					continue;

				if (diff_populate_filespec(r, p->one, NULL) < 0)
					continue;
				if (diff_populate_filespec(r, p->two, NULL) < 0) {
					diff_free_filespec_data(p->one);
					continue;
				}

				mf1.ptr = p->one->data;
				mf1.size = p->one->size;
				mf2.ptr = p->two->data;
				mf2.size = p->two->size;

				xpp.flags = xdl_opts;
				xecfg.hunk_func = collect_hunks_hunk;
				ecb.priv = &hc;

				err = xdi_diff(&mf1, &mf2, &xpp, &xecfg, &ecb);
				diff_free_filespec_data(p->one);
				diff_free_filespec_data(p->two);
				if (err)
					hc.overflow = 1;
				break; /* only one matching path */
			}

			diff_queue_clear(&diff_queued_diff);
			diff_free(&diffopt);

			if (hc.nr > 0 && hc.nr <= UINT32_MAX && !hc.overflow) {
				uint32_t k, offset;

				if (hdat.len > UINT32_MAX) {
					free(hc.hunks);
					free(entries);
					strbuf_release(&hdat);
					release_revisions(&revs);
					return error(_("diff-hunks hunk data exceeds 4 GB"));
				}
				offset = (uint32_t)hdat.len;

				strbuf_put_be32(&hdat, (uint32_t)hc.nr);
				for (k = 0; k < hc.nr; k++) {
					strbuf_put_be32(&hdat, hc.hunks[k].old_start);
					strbuf_put_be16(&hdat, hc.hunks[k].old_count);
					strbuf_put_be32(&hdat, hc.hunks[k].new_start);
					strbuf_put_be16(&hdat, hc.hunks[k].new_count);
				}

				ALLOC_GROW(entries, entries_nr + 1, entries_alloc);
				oidcpy(&entries[entries_nr].commit_oid, &commit->object.oid);
				oidcpy(&entries[entries_nr].parent_oid, &parent->item->object.oid);
				entries[entries_nr].hdat_offset = offset;
				entries_nr++;
			}
			free(hc.hunks);

			free_tree_buffer(repo_get_commit_tree(r, commit));
			free_tree_buffer(repo_get_commit_tree(r, parent->item));
		}
	}

	release_revisions(&revs);

	if (!entries_nr) {
		free(entries);
		strbuf_release(&hdat);
		return 0;
	}

	if (entries_nr > UINT32_MAX) {
		free(entries);
		strbuf_release(&hdat);
		return error(_("diff-hunks: too many entries (%"PRIuMAX")"),
			     (uintmax_t)entries_nr);
	}

	QSORT(entries, entries_nr, path_entry_cmp);

	if (ensure_diff_hunks_dir(r, path)) {
		free(entries);
		strbuf_release(&hdat);
		return -1;
	}

	fname = get_path_hunks_filename(r, path);
	hold_lock_file_for_update(&lk, fname, LOCK_DIE_ON_ERROR);
	f = hashfd(r->hash_algo, get_lock_file_fd(&lk),
		   get_lock_file_path(&lk));

	/* Register chunks */
	entry_size = path_index_entry_size(r->hash_algo);
	cf = init_chunkfile(f);
	add_chunk(cf, DHPF_CHUNKID_META, 4, write_meta_chunk);
	add_chunk(cf, DHPF_CHUNKID_INDEX, entries_nr * entry_size,
		  write_index_chunk);
	add_chunk(cf, DHPF_CHUNKID_DATA, hdat.len, write_data_chunk);

	/* Write header */
	hashwrite_be32(f, DIFF_HUNKS_SIGNATURE);
	hashwrite_u8(f, DIFF_HUNKS_VERSION);
	hashwrite_u8(f, oid_version(r->hash_algo));
	hashwrite_u8(f, get_num_chunks(cf));
	hashwrite_u8(f, 0); /* padding */

	/* Write TOC + chunk data */
	ctx.r = r;
	ctx.xdl_opts = xdl_opts;
	ctx.entries = entries;
	ctx.entries_nr = entries_nr;
	ctx.hdat = &hdat;
	write_chunkfile(cf, &ctx);

	finalize_hashfile(f, NULL, FSYNC_COMPONENT_DIFF_HUNKS,
			  CSUM_HASH_IN_STREAM | CSUM_FSYNC);
	commit_lock_file(&lk);

	free_chunkfile(cf);
	free(fname);
	free(entries);
	strbuf_release(&hdat);
	return 0;
}

/*
 * Generate per-path hunks for all files touched by reachable commits.
 * Walks history once to collect unique paths, then generates a per-path
 * file for each via write_path_hunks_file().
 *
 * This does N+1 revision walks (1 to collect paths, N to generate
 * each per-path file). A single-pass writer would be faster for large
 * repos but significantly more complex. For now, the serial per-path
 * approach is simple and correct; parallelism can be added externally
 * via xargs -P.
 */
int write_diff_hunks(struct repository *r, int xdl_opts)
{
	struct rev_info revs = { 0 };
	struct commit *commit;
	const char *argv[] = { "rev-list", "--all", NULL };
	struct setup_revision_opt rev_opt = { 0 };
	struct strmap paths = STRMAP_INIT;
	struct hashmap_iter iter;
	struct strmap_entry *entry;
	struct progress *progress;
	size_t nr_paths = 0, done = 0;

	repo_init_revisions(r, &revs, NULL);
	setup_revisions(ARRAY_SIZE(argv) - 1, argv, &revs, &rev_opt);

	if (prepare_revision_walk(&revs))
		return error(_("revision walk setup failed"));

	while ((commit = get_revision(&revs)) != NULL) {
		struct commit_list *parent;
		int j;

		if (!commit->parents)
			continue;

		for (parent = commit->parents; parent; parent = parent->next) {
			struct diff_options diffopt;

			if (repo_parse_commit(r, parent->item))
				continue;

			repo_diff_setup(r, &diffopt);
			diffopt.flags.recursive = 1;
			diffopt.detect_rename = 0;
			diffopt.output_format = DIFF_FORMAT_NO_OUTPUT;
			diffopt.no_free = 1;
			diff_setup_done(&diffopt);

			diff_tree_oid(&parent->item->object.oid,
				      &commit->object.oid, "", &diffopt);
			diffcore_std(&diffopt);

			for (j = 0; j < diff_queued_diff.nr; j++) {
				struct diff_filepair *p = diff_queued_diff.queue[j];
				if (DIFF_FILE_VALID(p->two) &&
				    S_ISREG(p->two->mode))
					strmap_put(&paths, p->two->path, NULL);
			}

			diff_queue_clear(&diff_queued_diff);
			diff_free(&diffopt);
		}
	}

	release_revisions(&revs);

	strmap_for_each_entry(&paths, &iter, entry)
		nr_paths++;

	if (!nr_paths) {
		strmap_clear(&paths, 0);
		return 0;
	}

	/*
	 * Clear commit marks left by the collection walk above.
	 * Each write_path_hunks_file() does its own revision walk
	 * and needs clean flags.
	 */
	repo_clear_commit_marks(r, ALL_REV_FLAGS);

	progress = start_delayed_progress(r,
		_("Generating per-path diff hunks"), nr_paths);

	strmap_for_each_entry(&paths, &iter, entry) {
		write_path_hunks_file(r, entry->key, xdl_opts);
		/*
		 * Each write_path_hunks_file() revision walk leaves
		 * commit marks that interfere with the next walk.
		 */
		repo_clear_commit_marks(r, ALL_REV_FLAGS);
		display_progress(progress, ++done);
	}

	stop_progress(&progress);
	strmap_clear(&paths, 0);
	return 0;
}
