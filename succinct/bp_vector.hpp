#pragma once

#include <vector>
#include <algorithm>
#include <limits>

#include <boost/range.hpp>

#include "rs_bit_vector.hpp"

namespace succinct {
    
    class bp_vector : public rs_bit_vector {
    public:
        bp_vector() 
            : rs_bit_vector()
        {}

        template <class Range>
        bp_vector(Range const& from,
		  bool with_select_hints = false,
		  bool with_select0_hints = false) 
            : rs_bit_vector(from, with_select_hints, with_select0_hints)
        {
            build_minmax_tree();
        }
	
        template <typename Visitor>
        void map(Visitor& visit) {
            rs_bit_vector::map(visit);
            visit
                (m_treesize, "m_treesize")
                (m_block_excess_minmax, "m_block_excess_minmax")
                (m_superblock_excess_minmax, "m_superblock_excess_minmax")
                ;
        }

        void swap(bp_vector& other) {
            rs_bit_vector::swap(other);
            std::swap(m_treesize, other.m_treesize);
            m_block_excess_minmax.swap(other.m_block_excess_minmax);
            m_superblock_excess_minmax.swap(other.m_superblock_excess_minmax);
        }
	    
        uint64_t find_open(uint64_t pos) const;
        uint64_t find_close(uint64_t pos) const;
        uint64_t enclose(uint64_t pos) const {
            assert((*this)[pos]);
            return find_open(pos);
        }
        typedef int32_t excess_t; // Allow at most 2^31 depth of the tree

    protected:

        typedef int16_t block_excess_t; // Block has to be at most 2^15 bits

        bool find_close_in_block(uint64_t pos, excess_t excess, uint64_t max_sub_blocks, uint64_t& ret) const;
        bool find_open_in_block(uint64_t pos, excess_t excess, uint64_t max_sub_blocks, uint64_t& ret) const;

        inline excess_t get_block_excess(uint64_t block) const {
            excess_t excess = 0;
            uint64_t block_pos = block * block_size * 64;
            excess = static_cast<excess_t>(2 * block_rank(block) - block_pos);
            assert(excess >= 0);
            return excess;
        }

        inline bool in_superblock_range(uint64_t superblock, excess_t excess) const {
            if (superblock < m_treesize) {
                return (excess >= m_superblock_excess_minmax[superblock * 2] &&
                        excess <= m_superblock_excess_minmax[superblock * 2 + 1]);
            } else {
                uint64_t block = superblock - m_treesize;
                excess_t block_excess = get_block_excess(block);
                return (excess >= block_excess + m_block_excess_minmax[block * 2] &&
                        excess <= block_excess + m_block_excess_minmax[block * 2 + 1]);
            }
        }
        
        inline uint64_t search_minmax_tree(uint64_t block, excess_t excess, uint8_t direction) const;

        void build_minmax_tree();

        uint64_t m_treesize;
        mapper::mappable_vector<block_excess_t> m_block_excess_minmax;
        mapper::mappable_vector<excess_t> m_superblock_excess_minmax;
    };
}
