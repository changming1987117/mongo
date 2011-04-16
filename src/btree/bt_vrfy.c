/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

/*
 * There's a bunch of stuff we pass around during verification, group it
 * together to make the code prettier.
 */
typedef struct {
	uint32_t  frags;			/* Total frags */
	bitstr_t *fragbits;			/* Frag tracking bit list */

	FILE	*stream;			/* Dump file stream */

	uint64_t fcnt;				/* Progress counter */

	uint64_t record_total;			/* Total record count */

	WT_BUF  *max_key;			/* Largest key */
	uint32_t max_addr;			/* Largest key page */
} WT_VSTUFF;

static int __wt_verify_addfrag(SESSION *, uint32_t, uint32_t, WT_VSTUFF *);
static int __wt_verify_checkfrag(SESSION *, WT_VSTUFF *);
static int __wt_verify_freelist(SESSION *, WT_VSTUFF *);
static int __wt_verify_overflow(
		SESSION *, WT_OVFL *, uint32_t, uint32_t, WT_VSTUFF *);
static int __wt_verify_overflow_page(SESSION *, WT_PAGE *, WT_VSTUFF *);
static int __wt_verify_row_int_key_order(
		SESSION *, WT_PAGE *, void *, WT_VSTUFF *);
static int __wt_verify_row_leaf_key_order(SESSION *, WT_PAGE *, WT_VSTUFF *);
static int __wt_verify_tree(SESSION *, WT_PAGE *, uint64_t, WT_VSTUFF *);

/*
 * __wt_btree_verify --
 *	Verify a Btree.
 */
int
__wt_btree_verify(SESSION *session)
{
	return (__wt_verify(session, NULL));
}

/*
 * __wt_verify --
 *	Verify a Btree, optionally dumping each page in debugging mode.
 */
int
__wt_verify(SESSION *session, FILE *stream)
{
	BTREE *btree;
	WT_CACHE *cache;
	WT_VSTUFF vstuff;
	int ret;

	btree = session->btree;
	cache = S2C(session)->cache;
	ret = 0;

	WT_CLEAR(vstuff);
	/*
	 * Allocate a bit array, where each bit represents a single allocation
	 * size piece of the file.   This is how we track the parts of the file
	 * we've verified.  Storing this on the heap seems reasonable: with a
	 * minimum allocation size of 512B, we would allocate 4MB to verify a
	 * 16GB file.  To verify larger files than we can handle this way, we'd
	 * have to write parts of the bit array into a disk file.
	 *
	 * !!!
	 * There's one portability issue -- the bitstring package uses "ints",
	 * not unsigned ints, or any fixed size.   If an "int" can't hold a
	 * big enough value, we could lose.   There's a check here to make we
	 * don't overflow.   I don't ever expect to see this error message, but
	 * better safe than sorry.
	 */
	vstuff.frags = WT_OFF_TO_ADDR(btree, btree->fh->file_size);
	if (vstuff.frags > INT_MAX) {
		__wt_errx(session, "file is too large to verify");
		ret = WT_ERROR;
		goto err;
	}
	WT_ERR(bit_alloc(session, vstuff.frags, &vstuff.fragbits));
	vstuff.stream = stream;
	WT_ERR(__wt_scr_alloc(session, 0, &vstuff.max_key));
	vstuff.max_addr = WT_ADDR_INVALID;

	/*
	 * The first sector of the file is the description record -- ignore
	 * it for now.
	 */
	bit_nset(vstuff.fragbits, 0, 0);

	/*
	 * During verification, we can only evict clean pages (otherwise we can
	 * race and verify pages not at all or more than once).  The variable
	 * volatile, and the eviction code checks before each eviction, so no
	 * further serialization is required.
	 */
	cache->only_evict_clean = 1;

	/* Verify the tree, starting at the root. */
	WT_ERR(__wt_verify_tree(
	    session, btree->root_page.page, (uint64_t)1, &vstuff));

	WT_ERR(__wt_verify_freelist(session, &vstuff));

	WT_ERR(__wt_verify_checkfrag(session, &vstuff));

	cache->only_evict_clean = 0;

err:	/* Wrap up reporting. */
	__wt_progress(session, NULL, vstuff.fcnt);

	/* Free allocated memory. */
	if (vstuff.fragbits != NULL)
		__wt_free(session, vstuff.fragbits);
	if (vstuff.max_key != NULL)
		__wt_scr_release(&vstuff.max_key);

	return (ret);
}

/*
 * __wt_verify_tree --
 *	Verify a tree, recursively descending through it in depth-first fashion.
 * The page argument was physically verified (so we know it's correctly formed),
 * and the in-memory version built.  Our job is to check logical relationships
 * in the page and in the tree.
 */
static int
__wt_verify_tree(
	SESSION *session,		/* Thread of control */
	WT_PAGE *page,			/* Page to verify */
	uint64_t parent_recno,		/* First record in this subtree */
	WT_VSTUFF *vs)			/* The verify package */
{
	WT_COL *cip;
	WT_COL_REF *cref;
	WT_REF *ref;
	WT_ROW_REF *rref;
	uint64_t recno;
	uint32_t i;
	int ret;

	ret = 0;

	/*
	 * The page's physical structure was verified when it was read into
	 * memory by the read server thread, and then the in-memory version
	 * of the page was built.   Now we make sure the page and tree are
	 * logically consistent.
	 *
	 * !!!
	 * The problem: (1) the read server has to build the in-memory version
	 * of the page because the read server is the thread that flags when
	 * any thread can access the page in the tree; (2) we can't build the
	 * in-memory version of the page until the physical structure is known
	 * to be OK, so the read server has to verify at least the physical
	 * structure of the page; (3) doing complete page verification requires
	 * reading additional pages (for example, overflow keys imply reading
	 * overflow pages in order to test the key's order in the page); (4)
	 * the read server cannot read additional pages because it will hang
	 * waiting on itself.  For this reason, we split page verification
	 * into a physical verification, which allows the in-memory version
	 * of the page to be built, and then a subsequent logical verification
	 * which happens here.
	 *
	 * Report progress every 10 pages.
	 */
	if (++vs->fcnt % 10 == 0)
		__wt_progress(session, NULL, vs->fcnt);

	/*
	 * Update frags list.
	 *
	 * XXX
	 * Verify currently walks the in-memory tree, which means we can see
	 * pages that have not yet been written to disk.  That's not going to
	 * work because in-flight pages don't map correctly to on-disk pages.
	 * Verify will only work correctly on a clean tree -- make sure that
	 * is what we're seeing.  This test can go away when verify takes a
	 * file argument instead of an already opened tree (or a tree that's
	 * known to be clean, assuming the upper-level is doing the open for
	 * us.)
	 */
	WT_ASSERT(session, page->addr != WT_ADDR_INVALID);
	WT_RET(__wt_verify_addfrag(session, page->addr, page->size, vs));

#ifdef HAVE_DIAGNOSTIC
	/* Optionally dump the page in debugging mode. */
	if (vs->stream != NULL)
		WT_RET(__wt_debug_page(session, page, NULL, vs->stream));
#endif

	/*
	 * Column-store key order checks: check the starting record number,
	 * then update the total record count.
	 */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		recno = page->u.col_int.recno;
		goto recno_chk;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		recno = page->u.col_leaf.recno;
recno_chk:	if (parent_recno != recno) {
			__wt_errx(session,
			    "page at addr %lu has a starting record of %llu "
			    "where the expected starting record was %llu",
			    (u_long)page->addr,
			    (unsigned long long)recno,
			    (unsigned long long)parent_recno);
			return (WT_ERROR);
		}
		break;
	}
	switch (page->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		vs->record_total += page->indx_count;
		break;
	case WT_PAGE_COL_RLE:
		recno = 0;
		WT_COL_FOREACH(page, cip, i)
			recno += WT_RLE_REPEAT_COUNT(WT_COL_PTR(page, cip));
		vs->record_total += recno;
		break;
	}

	/*
	 * Row-store leaf page key order check: it's a depth-first traversal,
	 * the first key on this page should be larger than any key previously
	 * seen.
	 */
	switch (page->type) {
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_verify_row_leaf_key_order(session, page, vs));
		break;
	}

	/*
	 * Check on-page overflow page references.
	 *
	 * There's a potential performance problem here: we read key overflow
	 * pages twice, once when checking the overflow page itself, and again
	 * when checking the key ordering.   It's a pain to combine the two
	 * tests (the page types with overflow items aren't exactly the same
	 * as the page types with ordered keys, and the underlying functions
	 * that instantiate (and decompress) overflow pages don't want to know
	 * anything about verification), and I don't want to keep the overflow
	 * keys in the cache, it's likely to be wasted space.  Until it's a
	 * problem, I'm going to assume the second read of the overflow key is
	 * satisfied in the operating system buffer cache, and not worry about
	 * it.  Table verify isn't likely to be a performance path anyway.
	 */
	switch (page->type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_verify_overflow_page(session, page, vs));
		break;
	}

	/* Check tree connections and recursively descend the tree. */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		/* For each entry in an internal page, verify the subtree. */
		WT_COL_REF_FOREACH(page, cref, i) {
			/*
			 * It's a depth-first traversal: this entry's starting
			 * record number should be 1 more than the total records
			 * reviewed to this point.
			 */
			if (cref->recno != vs->record_total + 1) {
				__wt_errx(session,
				    "page at addr %lu has a starting record of "
				    "%llu where the expected starting record "
				    "was %llu",
				    (u_long)page->addr,
				    (unsigned long long)cref->recno,
				    (unsigned long long)vs->record_total + 1);
				return (WT_ERROR);
			}

			/* cref references the subtree containing the record */
			ref = &cref->ref;
			WT_RET(__wt_page_in(session, page, ref, 1));
			ret = __wt_verify_tree(
			    session, ref->page, cref->recno, vs);
			__wt_hazard_clear(session, ref->page);
			if (ret != 0)
				return (ret);
		}
		break;
	case WT_PAGE_ROW_INT:
		/* For each entry in an internal page, verify the subtree. */
		WT_ROW_REF_FOREACH(page, rref, i) {
			/*
			 * It's a depth-first traversal: this entry's starting
			 * key should be larger than the largest key previously
			 * reviewed.
			 *
			 * The 0th key of any internal page is magic, and we
			 * can't test against it.
			 */
			if (WT_ROW_REF_SLOT(page, rref) != 0)
				WT_RET(__wt_verify_row_int_key_order(
				    session, page, rref, vs));

			/* rref references the subtree containing the record */
			ref = &rref->ref;
			WT_RET(__wt_page_in(session, page, ref, 1));
			ret = __wt_verify_tree(
			    session, ref->page, (uint64_t)0, vs);
			__wt_hazard_clear(session, ref->page);
			if (ret != 0)
				return (ret);
		}
		break;
	}
	return (0);
}

/*
 * __wt_verify_row_int_key_order --
 *	Compare a key on an internal page to the largest key we've seen so
 * far; update the largest key we've seen so far to that key.
 */
static int
__wt_verify_row_int_key_order(
    SESSION *session, WT_PAGE *page, void *rref, WT_VSTUFF *vs)
{
	BTREE *btree;
	WT_BUF *scratch;
	uint32_t size;
	int ret, (*func)(BTREE *, const WT_ITEM *, const WT_ITEM *);

	btree = session->btree;
	scratch = 0;
	func = btree->btree_compare;
	ret = 0;

	/*
	 * The maximum key must have been set, we updated it from a leaf page
	 * first.
	 */
	WT_ASSERT(session, vs->max_addr != WT_ADDR_INVALID);

	/* The key may require processing. */
	if (__wt_key_process(rref)) {
		WT_RET(__wt_scr_alloc(session, 0, &scratch));
		WT_RET(__wt_key_build(session, page, rref, scratch));
		rref = scratch;
	}

	/* Compare the key against the largest key we've seen so far. */
	if (func(btree, rref, (WT_ITEM *)vs->max_key) <= 0) {
		__wt_errx(session,
		    "the internal key in entry %lu on the page at addr %lu "
		    "sorts before a key appearing on page %lu",
		    (u_long)WT_ROW_REF_SLOT(page, rref),
		    (u_long)page->addr, (u_long)vs->max_addr);
		ret = WT_ERROR;
		WT_ASSERT(session, ret == 0);
	} else {
		/* Update the largest key we've seen to the key just checked. */
		vs->max_addr = page->addr;

		size = ((WT_ROW_REF *)rref)->size;
		if (size > vs->max_key->mem_size)
			WT_RET(__wt_buf_setsize(
			    session, vs->max_key, (size_t)size));
		memcpy(vs->max_key->mem, ((WT_ROW_REF *)rref)->key, size);
		vs->max_key->size = size;
	}

	if (rref == scratch)
		__wt_scr_release(&scratch);

	return (ret);
}

/*
 * __wt_verify_row_leaf_key_order --
 *	Compare the first key on a leaf page to the largest key we've seen so
 * far; update the largest key we've seen so far to the last key on the page.
 */
static int
__wt_verify_row_leaf_key_order(SESSION *session, WT_PAGE *page, WT_VSTUFF *vs)
{
	BTREE *btree;
	WT_BUF *scratch;
	uint32_t size;
	int ret, (*func)(BTREE *, const WT_ITEM *, const WT_ITEM *);
	void *key;

	btree = session->btree;
	scratch = NULL;
	func = btree->btree_compare;
	ret = 0;

	/*
	 * We visit our first leaf page before setting the maximum key (the 0th
	 * keys on the internal pages leading to the smallest leaf in the tree
	 * are all empty entries).
	 */
	if (vs->max_addr != WT_ADDR_INVALID) {
		/* The key may require processing. */
		key = page->u.row_leaf.d;
		if (__wt_key_process(key)) {
			WT_RET(__wt_scr_alloc(session, 0, &scratch));
			WT_RET(__wt_key_build(session, page, key, scratch));
			key = scratch;
		}

		/*
		 * Compare the key against the largest key we've seen so far.
		 *
		 * If we're comparing against a key taken from an internal page,
		 * we can compare equal (which is an expected path, the internal
		 * page key is often a copy of the leaf page's first key).  But,
		 * in the case of the 0th slot on an internal page, the last key
		 * we've seen was a key from a previous leaf page, and it's not
		 * OK to compare equally in that case.
		 */
		if (func(btree, key, (WT_ITEM *)vs->max_key) < 0) {
			__wt_errx(session,
			    "the first key on the page at addr %lu sorts "
			    "equal or less than a key appearing on page %lu",
			    (u_long)page->addr, (u_long)vs->max_addr);
			ret = WT_ERROR;
			WT_ASSERT(session, ret == 0);
		}

		if (key == scratch)
			__wt_scr_release(&scratch);
		if (ret != 0)
			return (ret);
	}

	/*
	 * Update the largest key we've seen to the last key on this page; the
	 * key may require processing.
	 */
	vs->max_addr = page->addr;

	key = page->u.row_leaf.d + (page->indx_count - 1);
	if (__wt_key_process(key))
		return (__wt_key_build(session, page, key, vs->max_key));

	size = ((WT_ROW *)key)->size;
	if (size > vs->max_key->mem_size)
		WT_RET(__wt_buf_setsize(session, vs->max_key, (size_t)size));
	memcpy(vs->max_key->mem, ((WT_ROW *)key)->key, size);
	vs->max_key->size = size;
	return (0);
}

/*
 * __wt_verify_overflow_page --
 *	Verify overflow items.
 */
static int
__wt_verify_overflow_page(SESSION *session, WT_PAGE *page, WT_VSTUFF *vs)
{
	WT_CELL *cell;
	WT_PAGE_DISK *dsk;
	uint32_t entry_num, i;

	dsk = page->XXdsk;

	if (dsk == NULL) {
		/*
		 * XXX
		 * This should all go away -- once we're only verifying "clean"
		 * trees or files, there will never be a case where we don't
		 * have a backing disk page.
		 */
		return (0);
	}

	/*
	 * Overflow items aren't "in-memory", they're on-disk.  Ignore the fact
	 * they might have been updated, that doesn't mean anything until page
	 * reconciliation writes them to disk.
	 *
	 * Walk the disk page, verifying overflow items.
	 */
	entry_num = 0;
	WT_CELL_FOREACH(dsk, cell, i) {
		++entry_num;
		switch (WT_CELL_TYPE(cell)) {
		case WT_CELL_KEY_OVFL:
		case WT_CELL_DATA_OVFL:
			WT_RET(__wt_verify_overflow(session,
			    WT_CELL_BYTE_OVFL(cell),
			    entry_num, page->addr, vs));
		}
	}
	return (0);
}

/*
 * __wt_verify_overflow --
 *	Read in an overflow page and check it.
 */
static int
__wt_verify_overflow(SESSION *session,
    WT_OVFL *ovfl, uint32_t entry_num, uint32_t page_ref_addr, WT_VSTUFF *vs)
{
	BTREE *btree;
	WT_PAGE_DISK *dsk;
	WT_BUF *scratch;
	uint32_t addr, size;
	int ret;

	btree = session->btree;
	scratch = NULL;
	ret = 0;

	addr = ovfl->addr;
	size = WT_HDR_BYTES_TO_ALLOC(btree, ovfl->size);

	/* Allocate enough memory to hold the overflow pages. */
	WT_RET(__wt_scr_alloc(session, size, &scratch));

	/* Read the page. */
	dsk = scratch->mem;
	WT_ERR(__wt_disk_read(session, dsk, addr, size));

	/*
	 * Verify the disk image -- this function would normally be called
	 * from the asynchronous read server, but overflow pages are read
	 * synchronously. Regardless, we break the overflow verification code
	 * into two parts, on-disk format checking and internal checking,
	 * just so it looks like all of the other page type checking.
	 */
	WT_ERR(__wt_verify_dsk_chunk(session, dsk, addr, size));

	/* Add the fragments. */
	WT_ERR(__wt_verify_addfrag(session, addr, size, vs));

	/*
	 * The only other thing to check is that the size we have in the page
	 * matches the size on the underlying overflow page.
	 */
	if (ovfl->size != dsk->u.datalen) {
		__wt_errx(session,
		    "overflow page reference in cell %lu on page at addr %lu "
		    "does not match the data size on the overflow page",
		    (u_long)entry_num, (u_long)page_ref_addr);
		ret = WT_ERROR;
	}

err:	__wt_scr_release(&scratch);

	return (ret);
}

/*
 * __wt_verify_freelist --
 *	Add the freelist fragments to the list of verified fragments.
 */
static int
__wt_verify_freelist(SESSION *session, WT_VSTUFF *vs)
{
	BTREE *btree;
	WT_FREE_ENTRY *fe;
	int ret;

	btree = session->btree;
	ret = 0;

	TAILQ_FOREACH(fe, &btree->freeqa, qa)
		WT_TRET(__wt_verify_addfrag(session, fe->addr, fe->size, vs));

	return (ret);
}

/*
 * __wt_verify_addfrag --
 *	Add the WT_PAGE's fragments to the list, and complain if we've already
 *	verified this chunk of the file.
 */
static int
__wt_verify_addfrag(
    SESSION *session, uint32_t addr, uint32_t size, WT_VSTUFF *vs)
{
	BTREE *btree;
	uint32_t frags, i;

	btree = session->btree;

	frags = WT_OFF_TO_ADDR(btree, size);
	for (i = 0; i < frags; ++i)
		if (bit_test(vs->fragbits, addr + i)) {
			__wt_errx(session,
			    "file fragment at addr %lu already verified",
			    (u_long)addr);
			return (WT_ERROR);
		}
	if (frags > 0)
		bit_nset(vs->fragbits, addr, addr + (frags - 1));
	return (0);
}

/*
 * __wt_verify_checkfrag --
 *	Verify we've checked all the fragments in the file.
 */
static int
__wt_verify_checkfrag(SESSION *session, WT_VSTUFF *vs)
{
	int ffc, ffc_start, ffc_end, frags, ret;

	frags = (int)vs->frags;		/* XXX: bitstring.h wants "ints" */
	ret = 0;

	/* Check for file fragments we haven't verified. */
	for (ffc_start = ffc_end = -1;;) {
		bit_ffc(vs->fragbits, frags, &ffc);
		if (ffc != -1) {
			bit_set(vs->fragbits, ffc);
			if (ffc_start == -1) {
				ffc_start = ffc_end = ffc;
				continue;
			}
			if (ffc_end == ffc - 1) {
				ffc_end = ffc;
				continue;
			}
		}
		if (ffc_start != -1) {
			if (ffc_start == ffc_end)
				__wt_errx(session,
				    "file fragment %d was never verified",
				    ffc_start);
			else
				__wt_errx(session,
				    "file fragments %d-%d were never verified",
				    ffc_start, ffc_end);
			ret = WT_ERROR;
		}
		ffc_start = ffc_end = ffc;
		if (ffc == -1)
			break;
	}
	return (ret);
}
