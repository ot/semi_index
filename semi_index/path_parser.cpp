#include "path_parser.hpp"

#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/phoenix_core.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>
#include <boost/spirit/include/phoenix_stl.hpp>
#include <iostream>

namespace json {
namespace path {

    namespace {

        namespace phoenix = boost::phoenix;
        namespace qi = boost::spirit::qi;
        namespace ascii = boost::spirit::ascii;
        
        template <typename Iterator>
        struct path_grammar : qi::grammar<Iterator, path_list_t(), ascii::space_type>
        {
            
            path_grammar() : path_grammar::base_type(path_list)
            {
                using qi::lexeme;
                using qi::int_;
                using ascii::char_;
		using phoenix::push_back;
                using namespace qi::labels;

		key = lexeme[+(char_ - '.' - '[' - ',') [_val += _1]];
		path = -((key [push_back(_val, _1)] >> -('[' >> int_ [push_back(_val, _1)] >> ']')) % '.');
		path_list = -(path % ',');
            }

            qi::rule<Iterator, std::string(), ascii::space_type> key;
            qi::rule<Iterator, path_t(), ascii::space_type> path;
            qi::rule<Iterator, path_list_t(), ascii::space_type> path_list;
        };
        
        typedef std::string::const_iterator string_iter_t;
        path_grammar<string_iter_t> path_parser;
    }  
    
    path_list_t parse(std::string const& s) 
    {
	path_list_t paths;
	string_iter_t first = s.begin();
        if (!phrase_parse(first, s.end(), path_parser, boost::spirit::ascii::space, paths) || first != s.end()) {
	    throw std::invalid_argument(std::string("Parsing error: unexpected \"") + std::string(first, s.end()) + std::string("\""));
	}
	return paths;
    }
}}
