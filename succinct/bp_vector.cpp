#include "bp_vector.hpp"

namespace succinct {
    
    namespace {
        class excess_tables
        {
        public:
            excess_tables() {
                for (int c = 0; c < 256; ++c) {
                    for (unsigned char i = 0; i < 9; ++i) {
                        m_fwd_pos[c][i] = 0;
                        m_bwd_pos[c][i] = 0;
                    }
                    // populate m_fwd_pos
                    int excess = 0;
                    for (char i = 0; i < 8; ++i) {
                        if ((c >> i) & 1) { // opening
                            ++excess;
                        } else { // closing
                            --excess;
                            if (excess < 0 && 
                                m_fwd_pos[c][-excess] == 0) { // not already found
                                m_fwd_pos[c][-excess] = i + 1;
                            }
                        }
                    }
                    m_excess[c] = excess;

                    // populate m_bwd_pos 
                    excess = 0;
                    for (unsigned char i = 0; i < 8; ++i) {
                        if ((c << i) & 128) { // opening
                            ++excess;
                            if (excess > 0 && 
                                m_bwd_pos[c][(unsigned char)excess] == 0) { // not already found
                                m_bwd_pos[c][(unsigned char)excess] = i + 1;
                            }
                        } else { // closing
                            --excess;
                        }
                    }
                }
            }

            uint8_t m_fwd_pos[256][9];
            uint8_t m_bwd_pos[256][9];
            int8_t m_excess[256];
        };
        
        const static excess_tables tables;

        inline bool find_close_in_word(uint64_t word, bp_vector::excess_t cur_exc, uint64_t& ret) 
        {
            assert(cur_exc > 0 && cur_exc <= 64);

            for (int i = 0; i < 8; ++i) {
                size_t shift = i * 8;
                uint8_t cur_byte = (word >> shift) & 0xFF;
                assert(cur_exc > 0);
                if (cur_exc <= 8) {
                    uint8_t bit_pos = tables.m_fwd_pos[cur_byte][cur_exc];
                    if (bit_pos) {
                        ret = shift + bit_pos - 1;
                        return true;
                    }
                }
                cur_exc += tables.m_excess[cur_byte];
            }
            return false;
        }

        inline bool find_open_in_word(uint64_t word, bp_vector::excess_t cur_exc, uint64_t& ret) {
            assert(cur_exc > 0 && cur_exc <= 64);

            for (int i = 0; i < 8; ++i) {
                size_t shift = (7 - i) * 8;
                uint8_t cur_byte = (word >> shift) & 0xFF;
                assert(cur_exc > 0);
                if (cur_exc <= 8) {
                    uint8_t bit_pos = tables.m_bwd_pos[cur_byte][cur_exc];
                    if (bit_pos) {
                        ret = shift + (8 - bit_pos);
                        return true;
                    }
                }
                cur_exc -= tables.m_excess[cur_byte];
            }
            return false;
        }

        inline bp_vector::excess_t sub_block_excess(uint64_t sub_ranks, uint64_t offset) {
            return static_cast<bp_vector::excess_t>(2 * (sub_ranks >> ((7 - offset) * 9) & 0x1FF) - offset * 64);
        }
    }

    inline bool bp_vector::find_close_in_block(uint64_t block_offset, bp_vector::excess_t excess, uint64_t start, uint64_t& ret) const {
        if (excess > excess_t((block_size - start) * 64)) {
            return false;
        }
        assert(excess > 0);
        uint64_t sub_ranks = sub_block_ranks(block_offset / block_size);
        excess_t excess_offset = sub_block_excess(sub_ranks, start);
        excess_t block_excess = excess - excess_offset;
        for (uint64_t sub_block_offset = start; sub_block_offset < block_size; ++sub_block_offset) {
            excess_t cur_exc = block_excess + sub_block_excess(sub_ranks, sub_block_offset);
            assert(cur_exc > 0);
            if (cur_exc <= 64) {
                uint64_t sub_block = block_offset + sub_block_offset;
                uint64_t word = m_bits[sub_block];
                if (find_close_in_word(word, cur_exc, ret)) {
                    ret += sub_block * 64;
                    return true;
                }
            }
        }
        return false;
    }
    
    uint64_t bp_vector::find_close(uint64_t pos) const
    {
        assert((*this)[pos]); // check there is an opening parenthesis in pos
        uint64_t ret = -1;
        // Search in current word
        uint64_t word_pos = (pos + 1) / 64;
        uint64_t shift = (pos + 1) % 64;
        uint64_t shifted_word = m_bits[word_pos] >> shift;
        // Pad with "open"
        uint64_t padded_word = shifted_word | (-!!shift & (~0ULL << (64 - shift)));

        excess_t word_exc = 1;
        if (find_close_in_word(padded_word, word_exc, ret)) {
            ret += pos + 1;
            return ret;
        }
        
        // Otherwise search in the local block
        uint64_t block = word_pos / block_size;
        uint64_t block_offset = block * block_size;
        uint64_t sub_block = word_pos % block_size;
        uint64_t local_rank = broadword::popcount(shifted_word);
        excess_t local_excess = static_cast<excess_t>((2 * local_rank) - (64 - shift));
        if (find_close_in_block(block_offset, local_excess + 1, sub_block + 1, ret)) {
            return ret;
        }

        // Otherwise, find the first appropriate block
        excess_t pos_excess = static_cast<excess_t>(2 * rank(pos) - pos);
        uint64_t found_block = search_minmax_tree(block, pos_excess, 1);
        uint64_t found_block_offset = found_block * block_size;
        excess_t found_block_excess = get_block_excess(found_block);

        // Search in the found block
        bool found = find_close_in_block(found_block_offset, found_block_excess - pos_excess, 0, ret);
        assert(found); (void)found;
        return ret;
    }

    inline bool bp_vector::find_open_in_block(uint64_t block_offset, bp_vector::excess_t excess, uint64_t start, uint64_t& ret) const {
        if (excess > excess_t(start * 64)) {
            return false;
        }
        assert(excess >= 0);
        uint64_t block = block_offset / block_size;
        uint64_t sub_ranks = sub_block_ranks(block);
        excess_t block_excess;
        if (start == block_size) {
            block_excess = static_cast<excess_t>(2 * (block_rank(block + 1) - block_rank(block)) - block_size * 64);
        } else {
            block_excess = sub_block_excess(sub_ranks, start);
        }
        excess_t last_excess_offset = block_excess;
        for (uint64_t sub_block_offset = start - 1; sub_block_offset + 1 > 0; --sub_block_offset) {
            excess_t cur_exc = excess - (block_excess - last_excess_offset);
            assert(cur_exc > 0);
            if (cur_exc <= 64) {
                uint64_t sub_block = block_offset + sub_block_offset;
                uint64_t word = m_bits[sub_block];
                if (find_open_in_word(word, cur_exc, ret)) {
                    ret += sub_block * 64;
                    return true;
                }
            }
            last_excess_offset = sub_block_excess(sub_ranks, sub_block_offset);
        }
        return false;
    }

    uint64_t bp_vector::find_open(uint64_t pos) const
    {
        assert(pos);
        uint64_t ret = -1;
        // Search in current word
        uint64_t word_pos = (pos / 64);
        uint64_t len = pos % 64;
        // Rest is padded with "close"
        uint64_t shifted_word = -!!len & (m_bits[word_pos] << (64 - len));

        excess_t word_exc = 1;
        if (find_open_in_word(shifted_word, word_exc, ret)) {
            ret += pos - 64;
            return ret;
        }

        // Otherwise search in the local block
        uint64_t block = word_pos / block_size;
        uint64_t block_offset = block * block_size;
        uint64_t sub_block = word_pos % block_size;
        uint64_t local_rank = broadword::popcount(shifted_word);
        excess_t local_excess = -static_cast<excess_t>((2 * local_rank) - len);
        if (find_open_in_block(block_offset, local_excess + 1, sub_block, ret)) {
            return ret;
        }

        // Otherwise, find the first appropriate block
        excess_t pos_excess = static_cast<excess_t>(2 * rank(pos) - pos) - 1;
        uint64_t found_block = search_minmax_tree(block, pos_excess, 0);
        uint64_t found_block_offset = found_block * block_size;
        // Since search is backwards, have to add the current block
        excess_t found_block_excess = get_block_excess(found_block + 1);

        // Search in the found block
        bool found = find_open_in_block(found_block_offset, found_block_excess - pos_excess, block_size, ret);
        assert(found); (void)found;
        return ret;
    }

    inline uint64_t bp_vector::search_minmax_tree(uint64_t block, excess_t excess, uint8_t direction) const {
        uint64_t cur_superblock = m_treesize + block;
        while (true) {
            assert(cur_superblock);
            bool going_back = (cur_superblock & 1) == direction;
            cur_superblock = cur_superblock / 2;
            if (!going_back) {
                uint64_t child = cur_superblock * 2 + direction;
                if (in_superblock_range(child, excess)) {
                    cur_superblock = child;
                    break;
                }
            }
        }

        while (cur_superblock < m_treesize) {
            uint64_t next_superblock = cur_superblock * 2 + (1 - direction);
            if (in_superblock_range(next_superblock, excess)) {
                cur_superblock = next_superblock;
                continue;
            }
            next_superblock += direction * 2 - 1;
            // if it is not one child, it must be the other
            assert(in_superblock_range(next_superblock, excess));
            cur_superblock = next_superblock;
        }
        return cur_superblock - m_treesize;
    }
    
    void bp_vector::build_minmax_tree() 
    {
	if (!size()) return;

        std::vector<block_excess_t> block_excess_minmax;
        block_excess_t cur_block_min = 0, cur_block_max = 0, cur_block_excess = 0;
        for (uint64_t sub_block = 0; sub_block < m_bits.size(); ++sub_block) {
            if (sub_block % block_size == 0) {
                if (sub_block) { 
                    block_excess_minmax.push_back(cur_block_min);
                    block_excess_minmax.push_back(cur_block_max);
                }
                cur_block_min = 0;
                cur_block_max = 0;
                cur_block_excess = 0;
            }
            uint64_t word = m_bits[sub_block];
            uint64_t mask = 1ULL;
            // for last block stop at bit boundary
            uint64_t n_bits = 
                (sub_block == m_bits.size() - 1 && size() % 64) 
                ? size() % 64 
                : 64;
            for (uint64_t i = 0; i < n_bits; ++i) {
                cur_block_excess += (word & mask) ? 1 : -1;

                cur_block_min = std::min(cur_block_min, cur_block_excess);
                cur_block_max = std::max(cur_block_max, cur_block_excess);

                mask <<= 1;
            }
        }
        // Flush last block minmaxs
        block_excess_minmax.push_back(cur_block_min);
        block_excess_minmax.push_back(cur_block_max);
        
        m_block_excess_minmax.steal(block_excess_minmax);

        size_t n_blocks = (size_t)num_blocks();
        assert(n_blocks == m_block_excess_minmax.size() / 2);

        size_t treesize = 1;
        while (treesize < n_blocks) treesize <<= 1;
        m_treesize = treesize;
        
        std::vector<excess_t> superblock_excess_minmax(treesize * 2);

        // Fill the lowest layer
        excess_t cur_super_min, cur_super_max;
        for (size_t i = 0; i < n_blocks; i += 2) {
            uint64_t block = i;
            excess_t block_excess = get_block_excess(block);
            cur_super_min = block_excess + m_block_excess_minmax[block * 2];
            cur_super_max = block_excess + m_block_excess_minmax[block * 2 + 1];
            
            if (i + 1 < n_blocks) {
                block = i + 1;
                block_excess = get_block_excess(block);
                cur_super_min = std::min(cur_super_min, block_excess + m_block_excess_minmax[block * 2]);
                cur_super_max = std::max(cur_super_max, block_excess + m_block_excess_minmax[block * 2 + 1]);
            }
            assert(cur_super_min >= 0);
            assert(cur_super_max >= 0);
             
            superblock_excess_minmax[((treesize + i) / 2) * 2]     = cur_super_min;
            superblock_excess_minmax[((treesize + i) / 2) * 2 + 1] = cur_super_max;
        }

        // Fill bottom-up the other layers
        for (size_t i = treesize - 2; i + 1 > 1; i -= 2) {
            superblock_excess_minmax[(i / 2) * 2]     = std::min(superblock_excess_minmax[i * 2],
                                                                 superblock_excess_minmax[(i + 1) * 2]);
            superblock_excess_minmax[(i / 2) * 2 + 1] = std::max(superblock_excess_minmax[i * 2 + 1],
                                                                 superblock_excess_minmax[(i + 1) * 2 + 1]);
        }
        m_superblock_excess_minmax.steal(superblock_excess_minmax);
    }
}
