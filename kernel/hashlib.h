// This is free and unencumbered software released into the public domain.
// 
// Anyone is free to copy, modify, publish, use, compile, sell, or
// distribute this software, either in source code form or as a compiled
// binary, for any purpose, commercial or non-commercial, and by any
// means.

// -------------------------------------------------------
// Written by Clifford Wolf <clifford@clifford.at> in 2014
// -------------------------------------------------------

#ifndef HASHLIB_H

#include <stdexcept>
#include <string>
#include <vector>

namespace hashlib {

const int hashtable_size_trigger = 2;
const int hashtable_size_factor = 3;

// The XOR version of DJB2
// (traditionally 5381 is used as starting value for the djb2 hash)
inline unsigned int mkhash(unsigned int a, unsigned int b) {
	return ((a << 5) + a) ^ b;
}

// The ADD version of DJB2
// (use this version for cache locality in b)
inline unsigned int mkhash_add(unsigned int a, unsigned int b) {
	return ((a << 5) + a) + b;
}

inline unsigned int mkhash_xorshift(unsigned int a) {
	if (sizeof(a) == 4) {
		a ^= a << 13;
		a ^= a >> 17;
		a ^= a << 5;
	} else if (sizeof(a) == 8) {
		a ^= a << 13;
		a ^= a >> 7;
		a ^= a << 17;
	} else
		throw std::runtime_error("mkhash_xorshift() only implemented for 32 bit and 64 bit ints");
	return a;
}

template<typename T> struct hash_ops {
	bool cmp(const T &a, const T &b) const {
		return a == b;
	}
	unsigned int hash(const T &a) const {
		return a.hash();
	}
};

template<> struct hash_ops<int> {
	template<typename T>
	bool cmp(T a, T b) const {
		return a == b;
	}
	unsigned int hash(unsigned int a) const {
		return a;
	}
};

template<> struct hash_ops<std::string> {
	bool cmp(const std::string &a, const std::string &b) const {
		return a == b;
	}
	unsigned int hash(const std::string &a) const {
		unsigned int v = 0;
		for (auto c : a)
			v = mkhash(v, c);
		return v;
	}
};

template<typename P, typename Q> struct hash_ops<std::pair<P, Q>> {
	bool cmp(std::pair<P, Q> a, std::pair<P, Q> b) const {
		return a == b;
	}
	unsigned int hash(std::pair<P, Q> a) const {
		hash_ops<P> p_ops;
		hash_ops<Q> q_ops;
		return mkhash(p_ops.hash(a.first), q_ops.hash(a.second));
	}
};

struct hash_cstr_ops {
	bool cmp(const char *a, const char *b) const {
		for (int i = 0; a[i] || b[i]; i++)
			if (a[i] != b[i])
				return false;
		return true;
	}
	unsigned int hash(const char *a) const {
		unsigned int hash = 5381;
		while (*a)
			 hash = mkhash(hash, *(a++));
		 return hash;
	}
};

struct hash_ptr_ops {
	bool cmp(const void *a, const void *b) const {
		return a == b;
	}
	unsigned int hash(const void *a) const {
		return (unsigned long)a;
	}
};

struct hash_obj_ops {
	bool cmp(const void *a, const void *b) const {
		return a == b;
	}
	template<typename T>
	unsigned int hash(const T *a) const {
		return a->hash();
	}
};

inline int hashtable_size(int min_size)
{
	static std::vector<int> primes = {
		23, 29, 37, 47, 59, 79, 101, 127, 163, 211, 269, 337, 431, 541, 677,
		853, 1069, 1361, 1709, 2137, 2677, 3347, 4201, 5261, 6577, 8231, 10289,
		12889, 16127, 20161, 25219, 31531, 39419, 49277, 61603, 77017, 96281,
		120371, 150473, 188107, 235159, 293957, 367453, 459317, 574157, 717697,
		897133, 1121423, 1401791, 1752239, 2190299, 2737937, 3422429, 4278037,
		5347553, 6684443, 8355563, 10444457, 13055587, 16319519, 20399411,
		25499291, 31874149, 39842687, 49803361, 62254207, 77817767, 97272239,
		121590311, 151987889, 189984863, 237481091, 296851369, 371064217
	};

	for (auto p : primes)
		if (p > min_size) return p;

	if (sizeof(int) == 4)
		throw std::length_error("hash table exceeded maximum size. use a ILP64 abi for larger tables.");

	for (auto p : primes)
		if (100129 * p > min_size) return 100129 * p;

	throw std::length_error("hash table exceeded maximum size.");
}

template<typename K, typename T, typename OPS = hash_ops<K>>
class dict
{
	struct entry_t
	{
		int link;
		std::pair<K, T> udata;

		entry_t() : link(-1) { }
		entry_t(const std::pair<K, T> &udata) : link(1), udata(udata) { }

		bool is_free() const { return link < 0; }
		int get_next() const { return (link > 0 ? link : -link) - 2; }
		bool get_last() const { return get_next() == -1; }
		void set_next_used(int next) { link = next + 2; }
		void set_next_free(int next) { link = -(next + 2); }
	};

	std::vector<int> hashtable;
	std::vector<entry_t> entries;
	int free_list, counter, begin_n;
	int begin_seek_count;
	OPS ops;

	void init()
	{
		free_list = -1;
		counter = 0;
		begin_n = -1;
		begin_seek_count = 0;
	}

	void init_from(const dict<K, T, OPS> &other)
	{
		hashtable.clear();
		entries.clear();

		counter = other.size();
		begin_n = counter - 1;
		entries.reserve(counter);

		for (auto &it : other)
			entries.push_back(entry_t(it));

		rehash();
	}

	int mkhash(const K &key) const
	{
		unsigned int hash = 0;
		if (!hashtable.empty())
			hash = ops.hash(key) % (unsigned int)(hashtable.size());
		return hash;
	}

	void upd_begin_n(bool do_refree = true)
	{
		if (begin_n < -1) {
			begin_n = -(begin_n+2);
			while (begin_n >= 0 && entries[begin_n].is_free()) { begin_seek_count++; begin_n--; }
			if (do_refree && begin_seek_count > int(entries.size() / 2)) refree();
		}
	}

	void refree()
	{
		free_list = -1;
		begin_n = -1;

		int last_free = -1;
		for (int i = 0; i < int(entries.size()); i++)
			if (entries[i].is_free()) {
				if (last_free != -1)
					entries[last_free].set_next_free(i);
				else
					free_list = i;
				last_free = i;
			} else
				begin_n = i;

		if (last_free != -1)
			entries[last_free].set_next_free(-1);

		begin_seek_count = 0;
	}

	void rehash()
	{
		upd_begin_n(false);
		entries.resize(begin_n + 1);

		free_list = -1;
		begin_n = -1;

		hashtable.clear();
		hashtable.resize(hashtable_size(entries.size() * hashtable_size_factor), -1);

		int last_free = -1;
		for (int i = 0; i < int(entries.size()); i++)
			if (entries[i].is_free()) {
				if (last_free != -1)
					entries[last_free].set_next_free(i);
				else
					free_list = i;
				last_free = i;
			} else {
				int hash = mkhash(entries[i].udata.first);
				entries[i].set_next_used(hashtable[hash]);
				hashtable[hash] = i;
				begin_n = i;
			}

		if (last_free != -1)
			entries[last_free].set_next_free(-1);

		begin_seek_count = 0;
	}

	int do_erase(const K &key, int hash)
	{
		int last_index = -1;
		int index = hashtable.empty() ? -1 : hashtable[hash];
		while (1) {
			if (index < 0)
				return 0;
			if (ops.cmp(entries[index].udata.first, key)) {
				if (last_index < 0)
					hashtable[hash] = entries[index].get_next();
				else
					entries[last_index].set_next_used(entries[index].get_next());
				entries[index].udata = std::pair<K, T>();
				entries[index].set_next_free(free_list);
				free_list = index;
				if (--counter == 0)
					clear();
				else if (index == begin_n)
					begin_n = -(begin_n+2);
				return 1;
			}
			last_index = index;
			index = entries[index].get_next();
		}
	}

	int lookup_index(const K &key, int hash) const
	{
		int index = hashtable.empty() ? -1 : hashtable[hash];
		while (1) {
			if (index < 0)
				return -1;
			if (ops.cmp(entries[index].udata.first, key))
				return index;
			index = entries[index].get_next();
		}
	}

	int insert_at(const std::pair<K, T> &value, int hash)
	{
		if (free_list < 0)
		{
			free_list = entries.size();
			entries.push_back(entry_t());

			if (entries.size() * hashtable_size_trigger > hashtable.size()) {
				int i = free_list;
				entries[i].udata = value;
				entries[i].set_next_used(0);
				begin_n = i;
				counter++;
				rehash();
				return i;
			}
		}

		int i = free_list;
		free_list = entries[i].get_next();
		entries[i].udata = value;
		entries[i].set_next_used(hashtable[hash]);
		hashtable[hash] = i;
		if ((begin_n < -1 && -(begin_n+2) <= i) || (begin_n >= -1 && begin_n <= i))
			begin_n = i;
		counter++;
		return i;
	}

public:
	class iterator
	{
		dict<K, T, OPS> *ptr;
		int index;
	public:
		iterator() { }
		iterator(dict<K, T, OPS> *ptr, int index) : ptr(ptr), index(index) { }
		iterator operator++() { do index--; while (index >= 0 && ptr->entries[index].is_free()); return *this; }
		bool operator==(const iterator &other) const { return index == other.index; }
		bool operator!=(const iterator &other) const { return index != other.index; }
		std::pair<K, T> &operator*() { return ptr->entries[index].udata; }
		std::pair<K, T> *operator->() { return &ptr->entries[index].udata; }
		const std::pair<K, T> &operator*() const { return ptr->entries[index].udata; }
		const std::pair<K, T> *operator->() const { return &ptr->entries[index].udata; }
	};

	class const_iterator
	{
		const dict<K, T, OPS> *ptr;
		int index;
	public:
		const_iterator() { }
		const_iterator(const dict<K, T, OPS> *ptr, int index) : ptr(ptr), index(index) { }
		const_iterator operator++() { do index--; while (index >= 0 && ptr->entries[index].is_free()); return *this; }
		bool operator==(const const_iterator &other) const { return index == other.index; }
		bool operator!=(const const_iterator &other) const { return index != other.index; }
		const std::pair<K, T> &operator*() const { return ptr->entries[index].udata; }
		const std::pair<K, T> *operator->() const { return &ptr->entries[index].udata; }
	};

	dict()
	{
		init();
	}

	dict(const dict<K, T, OPS> &other)
	{
		init_from(other);
	}

	dict(dict<K, T, OPS> &&other)
	{
		init();
		swap(other);
	}

	dict<K, T, OPS> &operator=(const dict<K, T, OPS> &other) {
		if (this != &other)
			init_from(other);
		return *this;
	}

	dict<K, T, OPS> &operator=(dict<K, T, OPS> &&other) {
		clear();
		swap(other);
		return *this;
	}

	dict(const std::initializer_list<std::pair<K, T>> &list)
	{
		init();
		for (auto &it : list)
			insert(it);
	}

	template<class InputIterator>
	dict(InputIterator first, InputIterator last)
	{
		init();
		insert(first, last);
	}

	template<class InputIterator>
	void insert(InputIterator first, InputIterator last)
	{
		for (; first != last; ++first)
			insert(*first);
	}

	std::pair<iterator, bool> insert(const std::pair<K, T> &value)
	{
		int hash = mkhash(value.first);
		int i = lookup_index(value.first, hash);
		if (i >= 0)
			return std::pair<iterator, bool>(iterator(this, i), false);
		i = insert_at(value, hash);
		return std::pair<iterator, bool>(iterator(this, i), true);
	}

	int erase(const K &key)
	{
		int hash = mkhash(key);
		return do_erase(key, hash);
	}

	iterator erase(iterator it)
	{
		int hash = mkhash(it->first);
		do_erase(it->first, hash);
		return ++it;
	}

	int count(const K &key) const
	{
		int hash = mkhash(key);
		int i = lookup_index(key, hash);
		return i < 0 ? 0 : 1;
	}

	iterator find(const K &key)
	{
		int hash = mkhash(key);
		int i = lookup_index(key, hash);
		if (i < 0)
			return end();
		return iterator(this, i);
	}

	const_iterator find(const K &key) const
	{
		int hash = mkhash(key);
		int i = lookup_index(key, hash);
		if (i < 0)
			return end();
		return const_iterator(this, i);
	}

	T& at(const K &key)
	{
		int hash = mkhash(key);
		int i = lookup_index(key, hash);
		if (i < 0)
			throw std::out_of_range("dict::at()");
		return entries[i].udata.second;
	}

	const T& at(const K &key) const
	{
		int hash = mkhash(key);
		int i = lookup_index(key, hash);
		if (i < 0)
			throw std::out_of_range("dict::at()");
		return entries[i].udata.second;
	}

	T& operator[](const K &key)
	{
		int hash = mkhash(key);
		int i = lookup_index(key, hash);
		if (i < 0)
			i = insert_at(std::pair<K, T>(key, T()), hash);
		return entries[i].udata.second;
	}

	void swap(dict<K, T, OPS> &other)
	{
		hashtable.swap(other.hashtable);
		entries.swap(other.entries);
		std::swap(free_list, other.free_list);
		std::swap(counter, other.counter);
		std::swap(begin_n, other.begin_n);
		std::swap(begin_seek_count, other.begin_seek_count);
	}

	bool operator==(const dict<K, T, OPS> &other) const {
		if (counter != other.counter)
			return false;
		if (counter == 0)
			return true;
		if (entries.size() < other.entries.size())
			for (auto &it : *this) {
				auto oit = other.find(it.first);
				if (oit == other.end() || oit->second != it.second)
					return false;
			}
		else
			for (auto &oit : other) {
				auto it = find(oit.first);
				if (it == end() || it->second != oit.second)
					return false;
			}
		return true;
	}

	bool operator!=(const dict<K, T, OPS> &other) const {
		return !(*this == other);
	}

	size_t size() const { return counter; }
	bool empty() const { return counter == 0; }
	void clear() { hashtable.clear(); entries.clear(); init(); }

	iterator begin() { upd_begin_n(); return iterator(this, begin_n); }
	iterator end() { return iterator(nullptr, -1); }

	const_iterator begin() const { ((dict*)this)->upd_begin_n(); return const_iterator(this, begin_n); }
	const_iterator end() const { return const_iterator(nullptr, -1); }
};

template<typename K, typename OPS = hash_ops<K>>
class pool
{
	struct entry_t
	{
		int link;
		K key;

		entry_t() : link(-1) { }
		entry_t(const K &key) : link(1), key(key) { }

		bool is_free() const { return link < 0; }
		int get_next() const { return (link > 0 ? link : -link) - 2; }
		bool get_last() const { return get_next() == -1; }
		void set_next_used(int next) { link = next + 2; }
		void set_next_free(int next) { link = -(next + 2); }
	};

	std::vector<int> hashtable;
	std::vector<entry_t> entries;
	int free_list, counter, begin_n;
	int begin_seek_count;
	OPS ops;

	void init()
	{
		free_list = -1;
		counter = 0;
		begin_n = -1;
		begin_seek_count = 0;
	}

	void init_from(const pool<K, OPS> &other)
	{
		hashtable.clear();
		entries.clear();

		counter = other.size();
		begin_n = counter - 1;
		entries.reserve(counter);

		for (auto &it : other)
			entries.push_back(entry_t(it));

		rehash();
	}

	int mkhash(const K &key) const
	{
		unsigned int hash = 0;
		if (!hashtable.empty())
			hash = ops.hash(key) % (unsigned int)(hashtable.size());
		return hash;
	}

	void upd_begin_n(bool do_refree = true)
	{
		if (begin_n < -1) {
			begin_n = -(begin_n+2);
			while (begin_n >= 0 && entries[begin_n].is_free()) { begin_seek_count++; begin_n--; }
			if (do_refree && begin_seek_count > int(entries.size() / 2)) refree();
		}
	}

	void refree()
	{
		free_list = -1;
		begin_n = -1;

		int last_free = -1;
		for (int i = 0; i < int(entries.size()); i++)
			if (entries[i].is_free()) {
				if (last_free != -1)
					entries[last_free].set_next_free(i);
				else
					free_list = i;
				last_free = i;
			} else
				begin_n = i;

		if (last_free != -1)
			entries[last_free].set_next_free(-1);

		begin_seek_count = 0;
	}

	void rehash()
	{
		upd_begin_n(false);
		entries.resize(begin_n + 1);

		free_list = -1;
		begin_n = -1;

		hashtable.clear();
		hashtable.resize(hashtable_size(entries.size() * hashtable_size_factor), -1);

		int last_free = -1;
		for (int i = 0; i < int(entries.size()); i++)
			if (entries[i].is_free()) {
				if (last_free != -1)
					entries[last_free].set_next_free(i);
				else
					free_list = i;
				last_free = i;
			} else {
				int hash = mkhash(entries[i].key);
				entries[i].set_next_used(hashtable[hash]);
				hashtable[hash] = i;
				begin_n = i;
			}

		if (last_free != -1)
			entries[last_free].set_next_free(-1);

		begin_seek_count = 0;
	}

	int do_erase(const K &key, int hash)
	{
		int last_index = -1;
		int index = hashtable.empty() ? -1 : hashtable[hash];
		while (1) {
			if (index < 0)
				return 0;
			if (ops.cmp(entries[index].key, key)) {
				if (last_index < 0)
					hashtable[hash] = entries[index].get_next();
				else
					entries[last_index].set_next_used(entries[index].get_next());
				entries[index].key = K();
				entries[index].set_next_free(free_list);
				free_list = index;
				if (--counter == 0)
					clear();
				else if (index == begin_n)
					begin_n = -(begin_n+2);
				return 1;
			}
			last_index = index;
			index = entries[index].get_next();
		}
	}

	int lookup_index(const K &key, int hash) const
	{
		int index = hashtable.empty() ? -1 : hashtable[hash];
		while (1) {
			if (index < 0)
				return -1;
			if (ops.cmp(entries[index].key, key))
				return index;
			index = entries[index].get_next();
		}
	}

	int insert_at(const K &key, int hash)
	{
		if (free_list < 0)
		{
			free_list = entries.size();
			entries.push_back(entry_t());

			if (entries.size() * hashtable_size_trigger > hashtable.size()) {
				int i = free_list;
				entries[i].key = key;
				entries[i].set_next_used(0);
				begin_n = i;
				counter++;
				rehash();
				return i;
			}
		}

		int i = free_list;
		free_list = entries[i].get_next();
		entries[i].key = key;
		entries[i].set_next_used(hashtable[hash]);
		hashtable[hash] = i;
		if ((begin_n < -1 && -(begin_n+2) <= i) || (begin_n >= -1 && begin_n <= i))
			begin_n = i;
		counter++;
		return i;
	}

public:
	class iterator
	{
		pool<K, OPS> *ptr;
		int index;
	public:
		iterator() { }
		iterator(pool<K, OPS> *ptr, int index) : ptr(ptr), index(index) { }
		iterator operator++() { do index--; while (index >= 0 && ptr->entries[index].is_free()); return *this; }
		bool operator==(const iterator &other) const { return index == other.index; }
		bool operator!=(const iterator &other) const { return index != other.index; }
		K &operator*() { return ptr->entries[index].key; }
		K *operator->() { return &ptr->entries[index].key; }
		const K &operator*() const { return ptr->entries[index].key; }
		const K *operator->() const { return &ptr->entries[index].key; }
	};

	class const_iterator
	{
		const pool<K, OPS> *ptr;
		int index;
	public:
		const_iterator() { }
		const_iterator(const pool<K, OPS> *ptr, int index) : ptr(ptr), index(index) { }
		const_iterator operator++() { do index--; while (index >= 0 && ptr->entries[index].is_free()); return *this; }
		bool operator==(const const_iterator &other) const { return index == other.index; }
		bool operator!=(const const_iterator &other) const { return index != other.index; }
		const K &operator*() const { return ptr->entries[index].key; }
		const K *operator->() const { return &ptr->entries[index].key; }
	};

	pool()
	{
		init();
	}

	pool(const pool<K, OPS> &other)
	{
		init_from(other);
	}

	pool(pool<K, OPS> &&other)
	{
		init();
		swap(other);
	}

	pool<K, OPS> &operator=(const pool<K, OPS> &other) {
		if (this != &other)
			init_from(other);
		return *this;
	}

	pool<K, OPS> &operator=(pool<K, OPS> &&other) {
		clear();
		swap(other);
		return *this;
	}

	pool(const std::initializer_list<K> &list)
	{
		init();
		for (auto &it : list)
			insert(it);
	}

	template<class InputIterator>
	pool(InputIterator first, InputIterator last)
	{
		init();
		insert(first, last);
	}

	template<class InputIterator>
	void insert(InputIterator first, InputIterator last)
	{
		for (; first != last; ++first)
			insert(*first);
	}

	std::pair<iterator, bool> insert(const K &key)
	{
		int hash = mkhash(key);
		int i = lookup_index(key, hash);
		if (i >= 0)
			return std::pair<iterator, bool>(iterator(this, i), false);
		i = insert_at(key, hash);
		return std::pair<iterator, bool>(iterator(this, i), true);
	}

	int erase(const K &key)
	{
		int hash = mkhash(key);
		return do_erase(key, hash);
	}

	iterator erase(iterator it)
	{
		int hash = mkhash(*it);
		do_erase(*it, hash);
		return ++it;
	}

	int count(const K &key) const
	{
		int hash = mkhash(key);
		int i = lookup_index(key, hash);
		return i < 0 ? 0 : 1;
	}

	iterator find(const K &key)
	{
		int hash = mkhash(key);
		int i = lookup_index(key, hash);
		if (i < 0)
			return end();
		return iterator(this, i);
	}

	const_iterator find(const K &key) const
	{
		int hash = mkhash(key);
		int i = lookup_index(key, hash);
		if (i < 0)
			return end();
		return const_iterator(this, i);
	}

	bool operator[](const K &key) const
	{
		int hash = mkhash(key);
		int i = lookup_index(key, hash);
		return i >= 0;
	}

	void swap(pool<K, OPS> &other)
	{
		hashtable.swap(other.hashtable);
		entries.swap(other.entries);
		std::swap(free_list, other.free_list);
		std::swap(counter, other.counter);
		std::swap(begin_n, other.begin_n);
		std::swap(begin_seek_count, other.begin_seek_count);
	}

	bool operator==(const pool<K, OPS> &other) const {
		if (counter != other.counter)
			return false;
		if (counter == 0)
			return true;
		if (entries.size() < other.entries.size())
			for (auto &it : *this) {
				auto oit = other.find(it.first);
				if (oit == other.end() || oit->second != it.second)
					return false;
			}
		else
			for (auto &oit : other) {
				auto it = find(oit.first);
				if (it == end() || it->second != oit.second)
					return false;
			}
		return true;
	}

	bool operator!=(const pool<K, OPS> &other) const {
		return !(*this == other);
	}

	size_t size() const { return counter; }
	bool empty() const { return counter == 0; }
	void clear() { hashtable.clear(); entries.clear(); init(); }

	iterator begin() { upd_begin_n(); return iterator(this, begin_n); }
	iterator end() { return iterator(nullptr, -1); }

	const_iterator begin() const { ((pool*)this)->upd_begin_n(); return const_iterator(this, begin_n); }
	const_iterator end() const { return const_iterator(nullptr, -1); }
};

} /* namespace hashlib */

#endif
