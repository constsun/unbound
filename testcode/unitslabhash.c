/*
 * testcode/unitslabhash.c - unit test for slabhash table.
 *
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */
/**
 * \file
 * Tests the locking LRU keeping hash table implementation.
 */

#include "config.h"
#include "testcode/unitmain.h"
#include "util/log.h"
#include "util/storage/slabhash.h"

/* --- test representation --- */
/** structure contains test key */
struct slabtestkey {
	/** the key id */
	int id;
	/** the entry */
	struct lruhash_entry entry;
};
/** structure contains test data */
struct slabtestdata {
	/** data value */
	int data;
};

/** sizefunc for lruhash */
static size_t test_sizefunc(void*, void*);
/** comparefunc for lruhash */
static int test_compfunc(void*, void*);
/** delkey for lruhash */
static void test_delkey(void*, void*);
/** deldata for lruhash */
static void test_deldata(void*, void*);
/* --- end test representation --- */

/** hash func, very bad to improve collisions, both high and low bits. */
static hashvalue_t myhash(int id) {
	hashvalue_t h = (hashvalue_t)id & 0x0f;
	h |= (h << 28);
	return h;
}

/** allocate new key, fill in hash. */
static struct slabtestkey* newkey(int id) {
	struct slabtestkey* k = (struct slabtestkey*)calloc(1, sizeof(struct slabtestkey));
	if(!k) fatal_exit("out of memory");
	k->id = id;
	k->entry.hash = myhash(id);
	k->entry.key = k;
	lock_rw_init(&k->entry.lock);
	return k;
}
/** new data el */
static struct slabtestdata* newdata(int val) {
	struct slabtestdata* d = (struct slabtestdata*)calloc(1, 
		sizeof(struct slabtestdata));
	if(!d) fatal_exit("out of memory");
	d->data = val;
	return d;
}
/** delete key */
static void delkey(struct slabtestkey* k) {	
	lock_rw_destroy(&k->entry.lock); free(k);}
/** delete data */
static void deldata(struct slabtestdata* d) {free(d);}

/** test hashtable using short sequence */
static void
test_short_table(struct slabhash* table) 
{
	struct slabtestkey* k = newkey(12);
	struct slabtestkey* k2 = newkey(14);
	struct slabtestdata* d = newdata(128);
	struct slabtestdata* d2 = newdata(129);
	
	k->entry.data = d;
	k2->entry.data = d2;

	slabhash_insert(table, myhash(12), &k->entry, d, NULL);
	slabhash_insert(table, myhash(14), &k2->entry, d2, NULL);
	
	unit_assert( slabhash_lookup(table, myhash(12), k, 0) == &k->entry);
	lock_rw_unlock( &k->entry.lock );
	unit_assert( slabhash_lookup(table, myhash(14), k2, 0) == &k2->entry);
	lock_rw_unlock( &k2->entry.lock );
	slabhash_remove(table, myhash(12), k);
	slabhash_remove(table, myhash(14), k2);
}

/** number of hash test max */
#define HASHTESTMAX 32

/** test adding a random element */
static void
testadd(struct slabhash* table, struct slabtestdata* ref[])
{
	int numtoadd = random() % HASHTESTMAX;
	struct slabtestdata* data = newdata(numtoadd);
	struct slabtestkey* key = newkey(numtoadd);
	key->entry.data = data;
	slabhash_insert(table, myhash(numtoadd), &key->entry, data, NULL);
	ref[numtoadd] = data;
}

/** test adding a random element */
static void
testremove(struct slabhash* table, struct slabtestdata* ref[])
{
	int num = random() % HASHTESTMAX;
	struct slabtestkey* key = newkey(num);
	slabhash_remove(table, myhash(num), key);
	ref[num] = NULL;
	delkey(key);
}

/** test adding a random element */
static void
testlookup(struct slabhash* table, struct slabtestdata* ref[])
{
	int num = random() % HASHTESTMAX;
	struct slabtestkey* key = newkey(num);
	struct lruhash_entry* en = slabhash_lookup(table, myhash(num), key, 0);
	struct slabtestdata* data = en? (struct slabtestdata*)en->data : NULL;
	if(en) {
		unit_assert(en->key);
		unit_assert(en->data);
	}
	if(0) log_info("lookup %d got %d, expect %d", num, en? data->data :-1,
		ref[num]? ref[num]->data : -1);
	unit_assert( data == ref[num] );
	if(en) { lock_rw_unlock(&en->lock); }
	delkey(key);
}

/** check integrity of hash table */
static void
check_lru_table(struct lruhash* table)
{
	struct lruhash_entry* p;
	size_t c = 0;
	lock_quick_lock(&table->lock);
	unit_assert( table->num <= table->size);
	unit_assert( table->size_mask == (int)table->size-1 );
	unit_assert( (table->lru_start && table->lru_end) ||
		(!table->lru_start && !table->lru_end) );
	unit_assert( table->space_used <= table->space_max );
	/* check lru list integrity */
	if(table->lru_start)
		unit_assert(table->lru_start->lru_prev == NULL);
	if(table->lru_end)
		unit_assert(table->lru_end->lru_next == NULL);
	p = table->lru_start;
	while(p) {
		if(p->lru_prev) {
			unit_assert(p->lru_prev->lru_next == p);
		}
		if(p->lru_next) {
			unit_assert(p->lru_next->lru_prev == p);
		}
		c++;
		p = p->lru_next;
	}
	unit_assert(c == table->num);

	/* this assertion is specific to the unit test */
	unit_assert( table->space_used == 
		table->num * test_sizefunc(NULL, NULL) );
	lock_quick_unlock(&table->lock);
}

/** check integrity of hash table */
static void
check_table(struct slabhash* table)
{
	size_t i;
	for(i=0; i<table->size; i++)
		check_lru_table(table->array[i]);
}

/** test adding a random element (unlimited range) */
static void
testadd_unlim(struct slabhash* table, struct slabtestdata** ref)
{
	int numtoadd = random() % (HASHTESTMAX * 10);
	struct slabtestdata* data = newdata(numtoadd);
	struct slabtestkey* key = newkey(numtoadd);
	key->entry.data = data;
	slabhash_insert(table, myhash(numtoadd), &key->entry, data, NULL);
	if(ref)
		ref[numtoadd] = data;
}

/** test adding a random element (unlimited range) */
static void
testremove_unlim(struct slabhash* table, struct slabtestdata** ref)
{
	int num = random() % (HASHTESTMAX*10);
	struct slabtestkey* key = newkey(num);
	slabhash_remove(table, myhash(num), key);
	if(ref)
		ref[num] = NULL;
	delkey(key);
}

/** test adding a random element (unlimited range) */
static void
testlookup_unlim(struct slabhash* table, struct slabtestdata** ref)
{
	int num = random() % (HASHTESTMAX*10);
	struct slabtestkey* key = newkey(num);
	struct lruhash_entry* en = slabhash_lookup(table, myhash(num), key, 0);
	struct slabtestdata* data = en? (struct slabtestdata*)en->data : NULL;
	if(en) {
		unit_assert(en->key);
		unit_assert(en->data);
	}
	if(0 && ref) log_info("lookup unlim %d got %d, expect %d", num, en ? 
		data->data :-1, ref[num] ? ref[num]->data : -1);
	if(data && ref) {
		/* its okay for !data, it fell off the lru */
		unit_assert( data == ref[num] );
	}
	if(en) { lock_rw_unlock(&en->lock); }
	delkey(key);
}

/** test with long sequence of adds, removes and updates, and lookups */
static void
test_long_table(struct slabhash* table) 
{
	/* assuming it all fits in the hastable, this check will work */
	struct slabtestdata* ref[HASHTESTMAX * 100];
	size_t i;
	memset(ref, 0, sizeof(ref));
	/* test assumption */
	if(0) slabhash_status(table, "unit test", 1);
	srandom(48);
	for(i=0; i<1000; i++) {
		/* what to do? */
		switch(random() % 4) {
			case 0:
			case 3:
				testadd(table, ref);
				break;
			case 1:
				testremove(table, ref);
				break;
			case 2:
				testlookup(table, ref);
				break;
			default:
				unit_assert(0);
		}
		if(0) slabhash_status(table, "unit test", 1);
		check_table(table);
	}

	/* test more, but 'ref' assumption does not hold anymore */
	for(i=0; i<1000; i++) {
		/* what to do? */
		switch(random() % 4) {
			case 0:
			case 3:
				testadd_unlim(table, ref);
				break;
			case 1:
				testremove_unlim(table, ref);
				break;
			case 2:
				testlookup_unlim(table, ref);
				break;
			default:
				unit_assert(0);
		}
		if(0) slabhash_status(table, "unlim", 1);
		check_table(table);
	}
}

/** structure to threaded test the lru hash table */
struct slab_test_thr {
	/** thread num, first entry. */
	int num;
	/** id */
	ub_thread_t id;
	/** hash table */
	struct slabhash* table;
};

/** main routine for threaded hash table test */
static void*
test_thr_main(void* arg) 
{
	struct slab_test_thr* t = (struct slab_test_thr*)arg;
	int i;
	log_thread_set(&t->num);
	for(i=0; i<1000; i++) {
		switch(random() % 4) {
			case 0:
			case 3:
				testadd_unlim(t->table, NULL);
				break;
			case 1:
				testremove_unlim(t->table, NULL);
				break;
			case 2:
				testlookup_unlim(t->table, NULL);
				break;
			default:
				unit_assert(0);
		}
		if(0) slabhash_status(t->table, "hashtest", 1);
		if(i % 100 == 0) /* because of locking, not all the time */
			check_table(t->table);
	}
	check_table(t->table);
	return NULL;
}

/** test hash table access by multiple threads. */
static void
test_threaded_table(struct slabhash* table)
{
	int numth = 10;
	struct slab_test_thr t[100];
	int i;

	for(i=1; i<numth; i++) {
		t[i].num = i;
		t[i].table = table;
		ub_thread_create(&t[i].id, test_thr_main, &t[i]);
	}

	for(i=1; i<numth; i++) {
		ub_thread_join(t[i].id);
	}
	if(0) slabhash_status(table, "hashtest", 1);
}

void slabhash_test()
{
	/* start very very small array, so it can do lots of table_grow() */
	/* also small in size so that reclaim has to be done quickly. */
	struct slabhash* table;
	printf("slabhash test\n");
	table = slabhash_create(4, 2, 5200, 
		test_sizefunc, test_compfunc, test_delkey, test_deldata, NULL);
	test_short_table(table);
	test_long_table(table);
	slabhash_delete(table);
	table = slabhash_create(4, 2, 5200, 
		test_sizefunc, test_compfunc, test_delkey, test_deldata, NULL);
	test_threaded_table(table);
	slabhash_delete(table);
}

static size_t test_sizefunc(void* ATTR_UNUSED(key), void* ATTR_UNUSED(data))
{
	return sizeof(struct slabtestkey) + sizeof(struct slabtestdata);
}

static int test_compfunc(void* key1, void* key2)
{
	struct slabtestkey* k1 = (struct slabtestkey*)key1;
	struct slabtestkey* k2 = (struct slabtestkey*)key2;
	if(k1->id == k2->id)
		return 0;
	if(k1->id > k2->id)
		return 1;
	return -1;
}

static void test_delkey(void* key, void* ATTR_UNUSED(arg))
{
	delkey((struct slabtestkey*)key);
}

static void test_deldata(void* data, void* ATTR_UNUSED(arg))
{
	deldata((struct slabtestdata*)data);
}
