#pragma once
#include <cstdint>
#include <algorithm>
#include "bool.h"
#include "format_with_commas.h"


class cache_ref_ht {
public:
	struct node {
		unsigned short filenum;
		unsigned int blocknum;		/* The cache block number within the database file. */
		int cacheblock_index;		/* Index of the cache block. */
		unsigned int ht_index;		/* Index into table[], for faster removal from hashtable. */
		node *prev;
		node *next;
	};

	unsigned int wang_hash1(unsigned int *key, int length, unsigned int initval)
	{
		unsigned int hash;
		int i;

		hash = initval;
		for (i = 0; i < length; ++i) {
			hash += key[i];
			hash += (hash << 16);
			hash ^= (hash >> 14);
			hash += (hash << 7);
			hash ^= (hash >> 3);
			hash += (hash << 5);
			hash ^= (hash >> 15);
		}
		return(hash);
	}

	unsigned int calc_ht_index(unsigned int filenum, unsigned int blocknum)
	{
		unsigned int key[] = {filenum, blocknum};
		unsigned int hashcode = wang_hash1(key, 2, 0);
		return(hashcode & (tablesize - 1));
	}

	int bytes_per_cacheblock(void)
	{
		return((int)std::round(1.5 * sizeof(table) + sizeof(node)));
	}

	node *lookup_internal(unsigned int ht_index, unsigned int filenum, unsigned int blocknum)
	{
		node *p;

		for (p = table[ht_index]; p != nullptr; p = p->next) {
			if (p->blocknum == blocknum && p->filenum == filenum)
				return(p);
		}
		return(nullptr);
	}

	node *lookup(unsigned int filenum, unsigned int blocknum)
	{
		unsigned int ht_index = calc_ht_index(filenum, blocknum);
		return(lookup_internal(ht_index, filenum, blocknum));
	}

	node *lookup(unsigned int filenum, unsigned int blocknum, unsigned int &ht_index)
	{
		ht_index = calc_ht_index(filenum, blocknum);
		return(lookup_internal(ht_index, filenum, blocknum));
	}

	/*
	 * If the block is not found, add it to the hashtable.
	 * In either case, return the pointer to the hashable node.
	 */
	node *add_node(unsigned int filenum, unsigned int blocknum, int cacheblock_index, unsigned int ht_index)
	{
		node *p;

		p = lookup_internal(ht_index, filenum, blocknum);
		if (p != nullptr)
			return(p);

		p = get_free_node();

		/* Add it to the hashtable. */
		p->prev = nullptr;
		p->next = table[ht_index];
		table[ht_index] = p;
		if (p->next)
			p->next->prev = p;
		p->filenum = filenum;
		p->blocknum = blocknum;
		p->cacheblock_index = cacheblock_index;
		p->ht_index = ht_index;
		return(p);
	}

	bool remove_node(node *p)
	{
		if (p == nullptr)
			return(false);

		/* Remove it from the hashtable. */
		if (p->prev == nullptr) {
			table[p->ht_index] = p->next;
			if (p->next != nullptr)
				p->next->prev = nullptr;
		}
		else {
			p->prev->next = p->next;
			if (p->next != nullptr)
				p->next->prev = p->prev;
		}

		/* Add node to the free list. */
		add_to_free_list(p);
		return(true);

	}

	/*
	 * If the block is not found, return false.
	 * If the block is found, return true and remove it from the hashtable.
	 */
	bool remove_node(unsigned int filenum, unsigned int blocknum)
	{
		unsigned int ht_index;
		node *p;

		ht_index = calc_ht_index(filenum, blocknum);
		p = lookup_internal(ht_index, filenum, blocknum);
		return(remove_node(p));
	}

	/* We don't use the .prev links for the free list because 
	 * we only add or remove at the head of the list.
	 */
	node *get_free_node(void)
	{
		node *p = free_list;
		free_list = p->next;
		return(p);
	}

	void add_to_free_list(node *p)
	{
		p->next = free_list;
		free_list = p;
	}

	void get_stats(int &used_entries, int &used_nodes)
	{
		used_entries = 0;
		for (int i = 0; i < tablesize; ++i) {
			if (table[i] != nullptr)
				++used_entries;
		}
		used_nodes = num_nodes - num_free();
	}

	bool open(int num_nodes_)
	{
		reset();
		num_nodes = num_nodes_;
		tablesize = next_pow2(num_nodes);
		if (num_nodes > 0) {
			table = (node **)std::malloc(tablesize * sizeof(table[0]));
			if (table == nullptr)
				return(true);

			nodes_base = (node *)std::malloc(num_nodes * sizeof(node));
			if (nodes_base == nullptr)
				return(true);

			/* Init each hashtable bucket linked list to empty. */
			for (int i = 0; i < tablesize; ++i)
				table[i] = nullptr;

			/* Init the free list of hashtable nodes. */
			for (int i = 0; i < num_nodes; ++i) {
				nodes_base[i].cacheblock_index = -1;
				nodes_base[i].filenum = 0;
				nodes_base[i].blocknum = 0;
				if (i != num_nodes - 1)
					nodes_base[i].next = nodes_base + i + 1;
				else
					nodes_base[i].next = nullptr;
			}
			free_list = nodes_base;
			return(false);
		}
		else {
			table = nullptr;
			nodes_base = nullptr;
			free_list = nullptr;
			return(true);
		}
	}

	void close(void)
	{
		if (table != nullptr)
			free(table);
		if (nodes_base != nullptr)
			free(nodes_base);
	}

	void reset(void)
	{
		nodes_base = nullptr;
		table = nullptr;
	}

	void test(void)
	{
		bool quick_test = true;
		unsigned int mults[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 15, 30};
		for (int i = 0; i < ARRAY_SIZE(mults); ++i) {
			if (test(mults[i] * 200000))
				return;
			if (quick_test)
				i += 4;
		}
	}

private:
	int tablesize;
	int num_nodes;
	unsigned int modulus_mask;
	node *nodes_base;			/* Used primarily for malloc and free. */
	node **table;
	node *free_list;


	unsigned int num_free(void)
	{
		unsigned int count;
		node *p;

		for (count = 0, p = free_list; p != nullptr; ++count, p = p->next)
			;
		return(count);
	}

	unsigned int next_pow2(unsigned int x)
	{
		return(1 << (1 + MSB(x)));
	}

	int get_list_length(int ht_index)
	{
		node *p;
		int count;

		count = 0;
		for (p = table[ht_index]; p != nullptr; p = p->next)
			++count;
		return(count);
	}

	
	bool test_config(unsigned int nfiles, unsigned int nblocks_per_file)
	{
		unsigned int filenum, blocknum, ht_index;
		int used_entries, used_nodes;

		for (filenum = 0; filenum < nfiles; ++filenum) {
			for (blocknum = 0; blocknum < nblocks_per_file; ++blocknum) {
				node *p = lookup(filenum, blocknum, ht_index);
				if (p != nullptr) {
					printf("lookup() returned non-null ptr unexpectedly\n");
					return(true);
				}
				p = add_node(filenum, blocknum, filenum + blocknum, ht_index);
				if (p == nullptr) {
					printf("add_node() returned nullptr unexpectedly\n");
					return(true);
				}
			}
		}

		/* Histogram the list length at each bin. */
		std::array<int64_t, 25> histogram;
		histogram.fill(0);
		for (int i = 0; i < tablesize; ++i) {
			int length = get_list_length(i);
			length = std::min(length, (int)histogram.size() - 1);
			++histogram[length];
		}

		unsigned int nfree, expected;
		nfree = num_free();
		expected = num_nodes - nfiles * nblocks_per_file;
		if (nfree != expected) {
			printf("incorrect count of free nodes, %u, expected %u\n", nfree, expected);
			return(true);
		}
		get_stats(used_entries, used_nodes);
		printf("hashtable: used entries %d of %d (%.2f%%), nfiles %u, nblocks_per_file %u\n", 
						used_entries, used_nodes, 100.0 * (double)used_entries/used_nodes, nfiles, nblocks_per_file);

		int64_t sum_lengths, sum_counts;
		sum_lengths = 0;
		sum_counts = 0;
		for (size_t i = 1; i < histogram.size(); ++i) {
			if (histogram[i] > 0) {
				sum_counts += histogram[i];
				sum_lengths += histogram[i] * i;
			}
		}
		printf("Avg. list length %.2f\n", (double)sum_lengths / (double)sum_counts);

		printf("Histogram of list lengths\n");
		for (size_t i = 0; i < histogram.size(); ++i)
			printf("%s  ", format_with_commas(histogram[i]).c_str());
		printf("\n");

		/* Reomve all entries. */
		for (filenum = 0; filenum < nfiles; ++filenum) {
			for (blocknum = 0; blocknum < nblocks_per_file; ++blocknum) {
				node *p = lookup(filenum, blocknum, ht_index);
				if (p == nullptr) {
					printf("lookup() returned nullptr unexpectedly\n");
					return(true);
				}
				if (p->cacheblock_index != p->filenum + p->blocknum) {
					printf("cacheblock_index is incorrect\n");
					return(true);
				}
				if ((blocknum & 1))
					remove_node(p);
				else
					remove_node(filenum, blocknum);
			}
		}

		/* Check that they are gone. */
		for (filenum = 0; filenum < nfiles; ++filenum) {
			for (blocknum = 0; blocknum < nblocks_per_file; ++blocknum) {
				node *p = lookup(filenum, blocknum, ht_index);
				if (p != nullptr) {
					printf("lookup() returned non-null ptr unexpectedly\n");
					return(true);
				}
			}
		}
		return(false);
	}

	bool test(int size)
	{
		int nfiles, nblocks_per_file;
		if (open(size))
			return(true);

		for (int i = 0; i < 12; ++i) {
			nfiles = 1 << i;
			nblocks_per_file = size / nfiles;
			if (test_config(nfiles, nblocks_per_file))
				return(true);
		}

		close();
		return(false);
	}
};


