#pragma once
#include <set>
#include <stdint.h>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/shared_ptr.hpp>
#include "succinct/mappable_vector.hpp"

namespace zrandom {
    void compress(std::string const& in_filename, std::string const& out_filename);

    class decompressor {
    public:
        decompressor(std::string const& filename);
        ~decompressor();
        
	typedef std::pair<size_t /* cur offset */, std::vector<char> /* cur block */> block_t;
	typedef boost::shared_ptr<const block_t> block_ptr_t;

	block_ptr_t read_block(size_t block_id) const;
	
	size_t block_size() const {
	    return m_block_size;
	}

        size_t num_blocks() const {
            return m_offsets.size();
        }
        
	class iterator
	    : public boost::iterator_facade<
	    iterator
	    , const char
	    , boost::random_access_traversal_tag
	    > {
	public:
	    iterator()
		: m_dec(0)
	    {}

	private:
	    friend class decompressor;
	    friend class boost::iterator_core_access;

	    iterator(const decompressor* dec, size_t pos)
		: m_dec(dec)
		, m_absolute_pos(pos)
		, m_cur_block(new block_ptr_t())
	    {}

	    bool equal(iterator const& other) const {
		return 
		    (this->m_dec == other.m_dec) &&
		    (this->m_absolute_pos == other.m_absolute_pos);
	    }

	    void increment() { 
		++m_absolute_pos;
	    }

	    void decrement() { 
		--m_absolute_pos;
	    }

	    void advance(difference_type n) {
		m_absolute_pos += n;
	    }

	    difference_type distance_to(iterator const& rhs) const {
		return rhs.m_absolute_pos - this->m_absolute_pos;
	    }

	    const char& dereference() const { 
		assert(m_dec);
		check_cur_block();
		return (*m_cur_block)->second[m_absolute_pos - (*m_cur_block)->first]; 
	    }

	    bool pos_in_block() const {
		return 
		    (m_absolute_pos >= (*m_cur_block)->first) &&
		    (m_absolute_pos < (*m_cur_block)->first + (*m_cur_block)->second.size());
	    }

	    void check_cur_block() const {
		if (*m_cur_block && pos_in_block()) {
		    // still in cur_block range, return
		    return;
		}
		
		size_t block_id = m_absolute_pos / m_dec->block_size();
		assert(block_id < m_dec->num_blocks());

		*m_cur_block = m_dec->read_block(block_id);
		assert(pos_in_block());
	    }
	    
	    const decompressor* m_dec;
	    size_t m_absolute_pos;

	    boost::shared_ptr<block_ptr_t> m_cur_block;
	}; 

	typedef iterator const_iterator;

	iterator begin() const {
	    return iterator(this, 0);
	}
	
	iterator end() const {
	    return iterator(this, m_original_size);
	}

    private:
        boost::iostreams::mapped_file_source m_mapped_file;
        const char* m_compressed_data;
        uint64_t m_original_size;
        uint64_t m_compressed_size;
        uint64_t m_block_size;
        succinct::mapper::mappable_vector<uint64_t> m_offsets;

	class cache;
	std::auto_ptr<cache> m_cache;

#ifdef ZRANDOM_PROFILE
        mutable uint64_t m_reads;
        mutable std::set<uint64_t> m_unique_reads;
#endif
    };
}
