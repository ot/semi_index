#include <iostream>
#include <fstream>
#include <vector>
#include <cassert>
#include <zlib.h>
#include <boost/unordered_map.hpp>

#include "succinct/mapper.hpp"
#include "zrandom.hpp"

namespace zrandom {

    static const int window_size = -15;
    static const size_t block_size = 16384;
    
    void compress(std::string const& in_filename, std::string const& out_filename)
    {
        std::ifstream fin(in_filename.c_str(), std::ios::binary);
        std::ofstream fout(out_filename.c_str(), std::ios::binary);
        
        std::vector<uint64_t> checkpoints;
	uint64_t original_size = 0;
        uint64_t compressed_size = 0;
        fout.write(reinterpret_cast<const char*>(&compressed_size), 
                   sizeof(compressed_size)); // placeholder for later
        assert(fout);

        z_stream strm;
        int ret;
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        ret = deflateInit2(&strm, 9, Z_DEFLATED,
                           window_size, 9, Z_DEFAULT_STRATEGY);
        assert(ret == Z_OK);

        unsigned char block[block_size];
        int flush = Z_FULL_FLUSH;
        do {
            fin.read((char*)block, block_size);
            strm.avail_in = fin.gcount();
            strm.next_in = block;
	    original_size += strm.avail_in;
            
            if (!fin) {
                flush = Z_FINISH;
            }

            checkpoints.push_back(compressed_size);
            do {
                unsigned char out[block_size];
                strm.avail_out = block_size;
                strm.next_out = out;
                ret = deflate(&strm, flush);
                assert(ret != Z_STREAM_ERROR);
                size_t have = block_size - strm.avail_out;
                fout.write((const char*)out, have);
                compressed_size += have;
                assert(fout);
            } while (strm.avail_out == 0);
        } while (flush != Z_FINISH);

        deflateEnd(&strm);

        succinct::mapper::mappable_vector<uint64_t> out_checkpoints;
        out_checkpoints.steal(checkpoints);
        succinct::mapper::freeze(original_size, fout);
        succinct::mapper::freeze(block_size, fout);
        succinct::mapper::freeze(out_checkpoints, fout);
        
        fout.seekp(0);
        fout.write(reinterpret_cast<const char*>(&compressed_size), 
                   sizeof(compressed_size));
    }

    class decompressor::cache {
    public:
	cache() 
	    : m_timestamp(0)
	{}

	block_ptr_t get(size_t key) {
	    cache_t::iterator found = m_cache.find(key);
	    if (found != m_cache.end()) {
		found->second.first = m_timestamp;
		block_ptr_t block = found->second.second;
		tick();
		return block;
	    }
	    return block_ptr_t();
	}

	void put(size_t key, block_ptr_t block) {
	    if (m_cache.size() == max_entries) {
		uint64_t min_timestamp = 0;
		cache_t::iterator min_iter = m_cache.end();
		for (cache_t::iterator iter = m_cache.begin(); iter != m_cache.end(); ++iter) {
		    if (min_iter == m_cache.end() || iter->first <= min_timestamp) {
			min_timestamp = iter->first;
			min_iter = iter;
		    }
		}
		m_cache.erase(min_iter);
	    }
	    assert(m_cache.size() < max_entries);
	    m_cache[key] = std::make_pair(uint64_t(m_timestamp), block);
	    tick();
	}

	void tick() {
	    ++m_timestamp;
	    if (!m_timestamp) {
		m_cache.clear();
	    }
	}

    private:
	static const size_t max_entries = 8;
	uint64_t m_timestamp;
	typedef boost::unordered_map<size_t, std::pair<uint64_t, block_ptr_t> > cache_t;
	cache_t m_cache;
    };


    decompressor::decompressor(std::string const& filename)
        : m_mapped_file(filename)
	, m_cache(new cache())
#ifdef ZRANDOM_PROFILE
        , m_reads(0)
#endif
    {
        const char* data = m_mapped_file.data();
        m_compressed_size = *(uint64_t*)data;
        data += sizeof(m_compressed_size);
        m_compressed_data = data;
        data += m_compressed_size;
        data += succinct::mapper::map(m_original_size, data);
        data += succinct::mapper::map(m_block_size, data);
        data += succinct::mapper::map(m_offsets, data, succinct::mapper::map_flags::warmup);
    }
    
    decompressor::~decompressor() 
    {
#ifdef ZRANDOM_PROFILE
        std::cerr << "**** Total reads: " << m_reads 
                  << ", Unique reads: " << m_unique_reads.size()
                  << std::endl;
#endif
    }
        
    decompressor::block_ptr_t decompressor::read_block(size_t block_id) const
    {
	block_ptr_t cached_block_ptr = m_cache->get(block_id);
	if (cached_block_ptr) {
	    return cached_block_ptr;
	}
	
	boost::shared_ptr<block_t> block_ptr(new block_t(block_id * block_size(), std::vector<char>())); // non-const ptr
	std::vector<char>& block = block_ptr->second;

        block.resize(m_block_size);
        
        z_stream strm;
        int ret;
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        ret = inflateInit2(&strm, window_size);
        assert(ret == Z_OK);

        size_t offset = m_offsets[block_id];
            
        strm.avail_in = m_compressed_size - offset;
        strm.next_in = (unsigned char*)(m_compressed_data + offset);

        strm.avail_out = m_block_size;
        strm.next_out = (unsigned char*)&block[0];

        ret = inflate(&strm, Z_BLOCK);
        assert(ret == Z_OK);
        block.resize(m_block_size - strm.avail_out);

        inflateEnd(&strm);

#ifdef ZRANDOM_PROFILE
        ++m_reads;
        m_unique_reads.insert(block_id);
#endif
	m_cache->put(block_id, block_ptr);
	return block_ptr;
    }
    
}

