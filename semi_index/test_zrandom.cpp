#define BOOST_TEST_MODULE zrandom
#include "succinct/test_common.hpp"

#include <fstream>
#include <boost/filesystem.hpp>

#include "zrandom.hpp"

BOOST_AUTO_TEST_CASE(zrandom_basic)
{
    using zrandom::compress;
    using zrandom::decompressor;

    std::string raw_filename = "_test_zrandom_raw_data";
    std::string compr_filename = raw_filename + ".gzra";

    {
        srand(42);
        std::ofstream raw_file_out(raw_filename.c_str(), std::ios::binary);
        for (size_t i = 0; i < 1000000; ++i) {
            raw_file_out.put(rand() & 0xFF);
        }
    }

    compress(raw_filename, compr_filename);
    
    {
        std::ifstream raw_file(raw_filename.c_str(), std::ios::binary);
        std::vector<char> raw_block;

        decompressor dc(compr_filename);
        for (size_t i = 0; i < dc.num_blocks(); ++i) {
            decompressor::block_ptr_t block;
            block = dc.read_block(i);
            raw_block.resize(block->second.size());
            BOOST_CHECK_MESSAGE(raw_file.read(&raw_block[0], raw_block.size()),
                                "Raw file read failed, i=" << i);
            BOOST_CHECK_MESSAGE(raw_block == block->second,
                                "Read block different from matched block, i=" << i << ", size=" << block->second.size() <<
                                ", r[0]=" << (int)raw_block[0] << ", u[0]=" << (int)block->second[0]);
        }

        size_t read = raw_file.tellg();
        raw_file.seekg(0, std::ios::end);
        BOOST_CHECK_EQUAL((size_t)raw_file.tellg(), read);

        raw_file.seekg(0);
        for (decompressor::iterator iter = dc.begin();
             iter != dc.end();
             ++iter) {
            char raw_c;
            raw_file.read(&raw_c, 1);
            BOOST_CHECK_EQUAL(raw_c, *iter);
        }
    }

    boost::filesystem::remove(raw_filename);
    boost::filesystem::remove(compr_filename);
}    

