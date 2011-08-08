#pragma once

#include <vector>

#include <boost/range.hpp>

#include "mappable_vector.hpp"
#include "broadword.hpp"

namespace succinct {

    namespace detail {
        inline size_t words_for(uint64_t n)
        {
            return (n + 64 - 1) / 64;
        }
    }

    class bit_vector;

    class bit_vector_builder : boost::noncopyable {
    public:

        typedef std::vector<uint64_t> bits_type;
        
        bit_vector_builder(uint64_t size = 0, bool init = 0)
            : m_size(size)
        {
            m_bits.resize(detail::words_for(size), -int64_t(init));
            if (size) {
                m_cur_word = &m_bits.back();
                // clear padding bits
                if (init && size % 64) {
                    *m_cur_word >>= 64 - (size % 64);
                }
            }
        }
        
        void reserve(uint64_t size) {
            m_bits.reserve(detail::words_for(size));
        }
        
        inline void push_back(bool b) {
            uint64_t pos_in_word = m_size % 64;
            if (pos_in_word == 0) {
                m_bits.push_back(0);
                m_cur_word = &m_bits.back();
            }
            *m_cur_word |= (uint64_t)b << pos_in_word;
            ++m_size;
        }

        inline void set(uint64_t i, bool b) {
            uint64_t word = i / 64;
            if (b) {
                m_bits[word] |= 1ULL << (i % 64);
            } else {
                m_bits[word] &= ~(1ULL << (i % 64));
            }
        }

        inline void append_bits(uint64_t bits, size_t len) {
            uint64_t pos_in_word = m_size % 64;
            m_size += len;
            if (pos_in_word == 0) {
                m_bits.push_back(bits);
            } else {
                *m_cur_word |= (bits << pos_in_word);
                if (len > 64 - pos_in_word) {
                    m_bits.push_back(bits >> (64 - pos_in_word));
                }
            }
            m_cur_word = &m_bits.back();
        }

        inline void zero_extend(uint64_t n) {
            m_size += n;
            uint64_t needed = detail::words_for(m_size) - m_bits.size();
            if (needed) {
                m_bits.insert(m_bits.end(), needed, 0);
                m_cur_word = &m_bits.back();
            }
        }
        
        bits_type& move_bits() {
            assert(detail::words_for(m_size) == m_bits.size());
            return m_bits;
        }
        
        uint64_t size() const {
            return m_size;
        }
        
    private:
        bits_type m_bits;
        uint64_t m_size;
        uint64_t* m_cur_word;
    };
    
    class bit_vector {
    public:
        bit_vector() 
            : m_size(0)
        {}

        template <class Range>
        bit_vector(Range const& from) {
            std::vector<uint64_t> bits;
            const uint64_t first_mask = uint64_t(1);
            uint64_t mask = first_mask;
            uint64_t cur_val = 0;
            m_size = 0;
            for (typename boost::range_const_iterator<Range>::type iter = boost::begin(from);
                 iter != boost::end(from);
                 ++iter) {
                if (*iter) {
                    cur_val |= mask;
                }
                mask <<= 1;
                m_size += 1;
                if (!mask) {
                    bits.push_back(cur_val);
                    mask = first_mask;
                    cur_val = 0;
                }
            }
            if (mask != first_mask) {
                bits.push_back(cur_val);
            }
            m_bits.steal(bits);
        }

        bit_vector(bit_vector_builder* from) {
            m_size = from->size();
            m_bits.steal(from->move_bits());
        }
	
        template <typename Visitor>
        void map(Visitor& visit) {
            visit
                (m_size, "m_size")
                (m_bits, "m_bits");
        }
	
        void swap(bit_vector& other) {
            std::swap(other.m_size, m_size);
            other.m_bits.swap(m_bits);
        }
	
        inline size_t size() const {
            return m_size;
        }

        inline bool operator[](uint64_t pos) const {
            assert(pos < m_size);
            uint64_t block = pos / 64;
            assert(block < m_bits.size());
            uint64_t shift = pos % 64;
            return (m_bits[block] >> shift) & 1;
        }

        inline uint64_t get_bits(uint64_t pos, uint64_t len) const {
            assert(pos + len <= m_size);
            if (!len) {
                return 0;
            }
            uint64_t block = pos / 64;
            uint64_t shift = pos % 64;
            uint64_t mask = -(len == 64) | ((1ULL << len) - 1);
            if (shift + len <= 64) {
                return m_bits[block] >> shift & mask;
            } else {
                return (m_bits[block] >> shift) | (m_bits[block + 1] << (64 - shift) & mask);
            }
        }

        inline uint64_t predecessor0(uint64_t pos) const {
            assert(pos < m_size);
            uint64_t block = pos / 64;
            uint64_t shift = 64 - pos % 64 - 1;
            uint64_t word = ~m_bits[block];
            word = (word << shift) >> shift;

            unsigned long ret;
            while (!broadword::msb(word, ret)) {
                assert(block);
                word = ~m_bits[--block];
            };
            return block * 64 + ret;
        }

        inline uint64_t successor0(uint64_t pos) const {
            assert(pos < m_size);
            uint64_t block = pos / 64;
            uint64_t shift = pos % 64;
            uint64_t word = (~m_bits[block] >> shift) << shift;

            unsigned long ret;
            while (!broadword::lsb(word, ret)) {
                ++block;
                assert(block < m_bits.size());
                word = ~m_bits[block];
            };
            return block * 64 + ret;
        }

        inline uint64_t predecessor1(uint64_t pos) const {
            assert(pos < m_size);
            uint64_t block = pos / 64;
            uint64_t shift = 64 - pos % 64 - 1;
            uint64_t word = m_bits[block];
            word = (word << shift) >> shift;

            unsigned long ret;
            while (!broadword::msb(word, ret)) {
                assert(block);
                word = m_bits[--block];
            };
            return block * 64 + ret;
        }

        inline uint64_t successor1(uint64_t pos) const {
            assert(pos < m_size);
            uint64_t block = pos / 64;
            uint64_t shift = pos % 64;
            uint64_t word = (m_bits[block] >> shift) << shift;

            unsigned long ret;
            while (!broadword::lsb(word, ret)) {
                ++block;
                assert(block < m_bits.size());
                word = m_bits[block];
            };
            return block * 64 + ret;
        }

    protected:
        size_t m_size;
        mapper::mappable_vector<uint64_t> m_bits;
    };
    
}
