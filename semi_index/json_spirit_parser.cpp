#include "json_spirit_parser.hpp"
#include "escape_table.hpp"

#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/phoenix_core.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>
#include <boost/spirit/include/phoenix_stl.hpp>
#include <boost/spirit/include/phoenix_fusion.hpp>
#include <boost/spirit/home/phoenix/bind/bind_member_function.hpp>
#include <boost/spirit/home/phoenix/function/function.hpp>
#include <boost/fusion/adapted/std_pair.hpp>
#include <boost/fusion/include/std_pair.hpp>

namespace {
    BOOST_SPIRIT_TERMINAL(quoted_string);
}

namespace boost { namespace spirit {
    template <>
    struct use_terminal<qi::domain, ::tag::quoted_string>
      : mpl::true_
    {};
}}

namespace {
    struct quoted_string_parser
      : boost::spirit::qi::primitive_parser<quoted_string_parser>
    {
        template <typename Context, typename Iterator>
        struct attribute
        {
            typedef std::string type;
        };
 
        template <typename Iterator, typename Context
                  , typename Skipper, typename Attribute>
        bool parse(Iterator& first, Iterator const& last,
		   Context&, Skipper const&, Attribute& attr) const
        {
            while (first != last) {
                char c = *first++;
		if (c == '"') {
		    return true;
		} else if (c == '\\') {
		    attr.push_back((char)json::parser::escape_table[(unsigned char)*first++]);
		} else {
		    attr.push_back(c);
		}
            }
            return false;
        }

        template <typename Context>
        boost::spirit::info what(Context&) const
        {
            return boost::spirit::info("quoted_string");
        }
    };
}

namespace boost { namespace spirit { namespace qi
{
    template <typename Modifiers>
    struct make_primitive< ::tag::quoted_string, Modifiers>
    {
        typedef quoted_string_parser result_type;
 
        result_type operator()(unused_type, unused_type) const
        {
            return result_type();
        }
    };
}}}

namespace json {
namespace parser {

    namespace {

        namespace phoenix = boost::phoenix;
        namespace qi = boost::spirit::qi;
        namespace ascii = boost::spirit::ascii;
        
        struct move_visitor : public boost::static_visitor<> {
        public:
            move_visitor(value& value)
                : value_(value)
            {}

            template <typename T>
            void operator()(T& operand) const
            {
                using std::swap;
                value_ = T();
                swap(operand, boost::get<T>(value_));
            }

        private: 
            move_visitor& operator=(move_visitor&);
            value& value_;
        };

        void move_to(value& val, value& to)
        {
            move_visitor mover(to);
            val.apply_visitor(mover);
        }

        struct move_into_impl {
            template <typename ValueType, typename Variant>
            struct result
            {
                typedef void type;
            };

            template <typename ValueType, typename Variant>
            void operator()(ValueType& val, Variant& to) const
            {
                using std::swap;
                to = ValueType();
                swap(boost::get<ValueType>(to), val);
            }
        };
        phoenix::function<move_into_impl> move_into;

        null_value null_obj;
        
        template <typename Iterator>
        struct json_grammar : qi::grammar<Iterator, value(), ascii::space_type>
        {
            
            json_grammar() : json_grammar::base_type(value_)
            {
                using qi::lexeme;
                using ascii::char_;
                using phoenix::at_c;
                using phoenix::swap;
                using namespace qi::labels;

                number = qi::double_ [_val = _1];
                string = '"' >> quoted_string [swap(_val, _1)];
                null_ = qi::string("null") [_val = null_obj];
                bool_ = qi::string("true") [_val = true] | qi::string("false") [_val = false];

                object_ = '{' >> -((string >> ':' >> value_) [phoenix::bind(&object::steal_append, _val, _1, _2)] % ',') >> '}';
                array_ = '[' >> -(value_ [phoenix::bind(&array::steal_append, _val, _1)] % ',') >> ']';

                value_ = 
                      string  [move_into(_1, _val)]
                    | number  [_val = _1]
                    | object_ [move_into(_1, _val)]
                    | array_  [move_into(_1, _val)]
                    | bool_   [_val = _1]
                    | null_   [_val = _1]
                    ;
            }

            qi::rule<Iterator, value(), ascii::space_type> value_;
            qi::rule<Iterator, object(), ascii::space_type> object_;
            qi::rule<Iterator, array(), ascii::space_type> array_;
            qi::rule<Iterator, double(), ascii::space_type> number;
            qi::rule<Iterator, std::string(), ascii::space_type> string;
            qi::rule<Iterator, bool(), ascii::space_type> bool_;
            qi::rule<Iterator, null_value(), ascii::space_type> null_;
        };
        
        typedef std::string::const_iterator string_iter_t;
        json_grammar<string_iter_t> json_string_parser;
        json_grammar<const char*> json_c_str_parser;
    }  
    

    void object::steal_append(std::string& k, value& v) 
    {
        move_to(v, (*this)[k]);
    }
            
    void array::steal_append(value& val) 
    {
        push_back(new value());
        move_to(val, back());
    }

    bool parse(std::string const& s, value& val) 
    {
        string_iter_t first = s.begin(), last = s.end();
        bool ret = phrase_parse(first, last, json_string_parser, boost::spirit::ascii::space, val);
        return ret;
    }


    bool parse(const char* first, const char* last, value& val) 
    {
        bool ret = phrase_parse(first, last, json_c_str_parser, boost::spirit::ascii::space, val);
        return ret;
    }
}
}
