#include <stdio.h>
#include <boost/foreach.hpp>
#include <boost/iterator/iterator_facade.hpp>

#include "jsoncpp/json/json.h"

#include "mongodb/db/jsobj.h"
#include "mongodb/db/json.h"

#include "succinct/util.hpp"
#include "succinct/mapper.hpp"

#include "semi_index/json_semi_index.hpp"
#include "semi_index/path_parser.hpp"
#include "semi_index/zrandom.hpp"

using json::path::path_element_t;
using json::path::path_t;
using json::path::path_list_t;
using succinct::util::fast_getline;

void nop_stream()
{
    std::string line;
    while (fast_getline(line)) {
	
    }
}

void naive_parse_stream(const char* paths_spec)
{
    path_list_t paths = json::path::parse(paths_spec);
    Json::Reader reader;
    Json::FastWriter writer;
    std::string line;
    while (fast_getline(line)) {
	Json::Value root;
	reader.parse(line, root);
	fwrite("[", 1, 1, stdout);
	Json::Value const* accessor;
	bool first = true;
	BOOST_FOREACH(path_t const& path, paths) {
	    accessor = &root;
	    BOOST_FOREACH(path_element_t const& element, path) {
		const std::string* key;
		const int* idx;
		if ((key = boost::get<std::string>(&element))) {
		    if (!accessor->isObject()) break;
		    accessor = &(*accessor)[*key];
		} else if ((idx = boost::get<int>(&element))) {
		    if (!accessor->isArray()) break;
		    if (*idx >= 0) {
			accessor = &(*accessor)[*idx];
		    } else {
			accessor = &(*accessor)[accessor->size() + *idx];
		    }
		}
	    }
	    if (!first) {
		fwrite(",", 1, 1, stdout);
	    } else {
		first = false;
	    }
	    
	    std::string out;
	    writer.write(*accessor).swap(out);
	    fwrite(out.c_str(), out.size() - 1, 1, stdout); // -1 to remove the final \n
	}
	fwrite("]\n", 2, 1, stdout);
    }
}

using semi_index::json_semi_index;

void si_parse_stream(const char* paths_spec)
{
    path_list_t paths = json::path::parse(paths_spec);
    std::string line;
    
    while (fast_getline(line)) {
	json_semi_index index(std::make_pair(&line, &line + 1));
	json_semi_index::accessor accessor, root = index.get_cursor().get_accessor(line.c_str());
	fwrite("[", 1, 1, stdout);
	bool first = true;
	BOOST_FOREACH(path_t const& path, paths) {
	    accessor = root.get_path(path);
	    
	    if (!first) {
		fwrite(",", 1, 1, stdout);
	    } else {
		first = false;
	    }
	    
	    if (accessor.is_valid) {
		json_semi_index::accessor::range_t r = accessor.get_range();
		fwrite(line.c_str() + r.first, r.second - r.first, 1, stdout);
	    } else {
		fwrite("null", 4, 1, stdout);
	    }
	}
	fwrite("]\n", 2, 1, stdout);
    }
}

void si_save(const char* index_file)
{
    using succinct::util::lines;
    json_semi_index index(lines(stdin));
    succinct::mapper::size_tree_of(index)->dump();
    succinct::mapper::freeze(index, index_file);
}

void saved_si_parse_stream(const char* index_file, const char* paths_spec)
{
    json_semi_index index;
    boost::iostreams::mapped_file_source m(index_file);
    succinct::mapper::map(index, m);
    json_semi_index::cursor cursor = index.get_cursor();

    path_list_t paths = json::path::parse(paths_spec);
    std::string line;
    
    while (fast_getline(line)) {
	json_semi_index::accessor accessor, root = cursor.get_accessor(line.c_str());
	cursor = cursor.next();
	fwrite("[", 1, 1, stdout);
	bool first = true;
	BOOST_FOREACH(path_t const& path, paths) {
	    accessor = root.get_path(path);
	    
	    if (!first) {
		fwrite(",", 1, 1, stdout);
	    } else {
		first = false;
	    }
	    
	    if (accessor.is_valid) {
		json_semi_index::accessor::range_t r = accessor.get_range();
		fwrite(line.c_str() + r.first, r.second - r.first, 1, stdout);
	    } else {
		fwrite("null", 4, 1, stdout);
	    }
	}
	fwrite("]\n", 2, 1, stdout);
    }
}

void saved_si_parse_mapped(const char* json_file, const char* index_file, const char* paths_spec)
{
    boost::iostreams::mapped_file_source json_map(json_file);
    const char* json = json_map.data();

    json_semi_index index;
    boost::iostreams::mapped_file_source m(index_file);
    succinct::mapper::map(index, m, succinct::mapper::map_flags::warmup);
    json_semi_index::cursor cursor = index.get_cursor();

    path_list_t paths = json::path::parse(paths_spec);
    
    while (!(cursor == json_semi_index::cursor())) {
	const char* line = json + cursor.get_offset();
	json_semi_index::accessor accessor, root = cursor.get_accessor(line);
	cursor = cursor.next();
	fwrite("[", 1, 1, stdout);
	bool first = true;
	BOOST_FOREACH(path_t const& path, paths) {
	    accessor = root.get_path(path);
	    
	    if (!first) {
		fwrite(",", 1, 1, stdout);
	    } else {
		first = false;
	    }
	    
	    if (accessor.is_valid) {
		json_semi_index::accessor::range_t r = accessor.get_range();
		fwrite(line + r.first, r.second - r.first, 1, stdout);
	    } else {
		fwrite("null", 4, 1, stdout);
	    }
	}
	fwrite("]\n", 2, 1, stdout);
    }
}

void saved_si_parse_compressed(const char* json_compressed_file, const char* index_file, const char* paths_spec)
{
    zrandom::decompressor json_dec(json_compressed_file);
    zrandom::decompressor::iterator json = json_dec.begin();

    typedef semi_index::json_semi_index_base<zrandom::decompressor::iterator> json_semi_index_z;
    json_semi_index_z index;
    boost::iostreams::mapped_file_source m(index_file);
    succinct::mapper::map(index, m);
    json_semi_index_z::cursor cursor = index.get_cursor();

    path_list_t paths = json::path::parse(paths_spec);
    
    while (!(cursor == json_semi_index_z::cursor())) {
	zrandom::decompressor::iterator line = json + cursor.get_offset();
	json_semi_index_z::accessor accessor, root = cursor.get_accessor(line);
	cursor = cursor.next();
	fwrite("[", 1, 1, stdout);
	bool first = true;
	BOOST_FOREACH(path_t const& path, paths) {
	    accessor = root.get_path(path);
	    
	    if (!first) {
		fwrite(",", 1, 1, stdout);
	    } else {
		first = false;
	    }
	    
	    if (accessor.is_valid) {
		json_semi_index_z::accessor::range_t r = accessor.get_range();
		std::string val(line + r.first, line + r.second);
		fwrite(val.c_str(), val.size(), 1, stdout);
	    } else {
		fwrite("null", 4, 1, stdout);
	    }
	}
	fwrite("]\n", 2, 1, stdout);
    }
}

void bson_save(const char* output_file)
{
    succinct::util::auto_file fout(output_file, "wb");
    std::string line;
    while (succinct::util::fast_getline(line, stdin, true)) {
        mongo::BSONObj bsobj = mongo::fromjson(line);
        fwrite(bsobj.objdata(), bsobj.objsize(), 1, fout.get());
    }
}

void bson_parse_mapped(const char* bson_file, const char* paths_spec)
{
    boost::iostreams::mapped_file_source bson_map(bson_file);
    const char* bson = bson_map.data();
    const char* bson_end = bson + bson_map.size();

    path_list_t paths = json::path::parse(paths_spec);
    
    const char* cur = bson;
    while (cur != bson_end) {
        mongo::BSONObj bsobj(cur);
        assert(bsobj.isValid());
        cur += bsobj.objsize();
        assert(cur <= bson_end);

	fwrite("[", 1, 1, stdout);
	bool first = true;
	BOOST_FOREACH(path_t const& path, paths) {
	    if (!first) {
		fwrite(",", 1, 1, stdout);
	    } else {
		first = false;
	    }
	    
            mongo::BSONObj cur_obj = bsobj;
            mongo::BSONElement cur_elem;
            bool cur_is_array = false;

	    BOOST_FOREACH(path_element_t const& element, path) {
		const std::string* key;
		const int* idx;
		if ((key = boost::get<std::string>(&element))) {
		    cur_elem = cur_obj[*key];
		} else if ((idx = boost::get<int>(&element))) {
                    if (!cur_is_array) {
                        cur_elem = mongo::BSONElement();
                        break;
                    }
		    if (*idx >= 0) {
                        cur_elem = cur_obj[*idx];
		    } else {
                        std::vector<mongo::BSONElement> elems;
                        cur_obj.elems(elems);
                        int fwd_idx = elems.size() + *idx;
                        if (fwd_idx < 0) {
                            cur_elem = mongo::BSONElement();
                            break;
                        } else {
                            cur_elem = elems[fwd_idx];
                            assert((int)mongo::stringToNum(cur_elem.fieldName()) == fwd_idx);
                        }
		    }
		}
                if (cur_elem.isABSONObj()) {
                    cur_obj = cur_elem.embeddedObject();
                    cur_is_array = (cur_elem.type() == mongo::Array);
                } else {
                    cur_obj = mongo::BSONObj();
                }
	    }

	    if (cur_elem.ok()) {
                std::string js = cur_elem.jsonString(mongo::Strict);
		fwrite(js.c_str(), js.size(), 1, stdout);
	    } else {
		fwrite("null", 4, 1, stdout);
	    }
	}
	fwrite("]\n", 2, 1, stdout);
        
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
	std::cerr << "No command given" << std::endl;
	exit(1);
    }
	
    std::string cmd(argv[1]);
    if (cmd == "nop_stream") {
	nop_stream();
    } else if (cmd == "naive_parse_stream") {
	naive_parse_stream(argv[2]);
    } else if (cmd == "si_parse_stream") {
	si_parse_stream(argv[2]);
    } else if (cmd == "si_save") {
	si_save(argv[2]);
    } else if (cmd == "saved_si_parse_stream") {
	saved_si_parse_stream(argv[2], argv[3]);
    } else if (cmd == "saved_si_parse_mapped") {
	saved_si_parse_mapped(argv[2], argv[3], argv[4]);
    } else if (cmd == "compress_file") {
        zrandom::compress(argv[2], argv[3]);
    } else if (cmd == "saved_si_parse_compressed") {
	saved_si_parse_compressed(argv[2], argv[3], argv[4]);
    } else if (cmd == "bson_save") {
	bson_save(argv[2]);
    } else if (cmd == "bson_parse_mapped") {
	bson_parse_mapped(argv[2], argv[3]);
    } else {
	std::cerr << "Unknown command: " << cmd << std::endl;
	exit(1);
    }
    
    return 0;
}
