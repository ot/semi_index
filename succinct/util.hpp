#pragma once

#include <string>
#include <stdexcept>
#include <fstream>
#include <list>

#include <stdint.h>

namespace succinct {
namespace util {

    void trim_newline_chars(std::string& s)
    {
        size_t l = s.size();
        while (l && (s[l-1] == '\r' ||
                     s[l-1] == '\n')) {
            --l;
        }
        s.resize(l);
    }

    // this is considerably faster than std::getline
    bool fast_getline(std::string& line, FILE* input = stdin, bool trim_newline = false)
    {
        line.clear();
        static const size_t max_buffer = 65536;
        char buffer[max_buffer];
        bool done = false;
        while (!done) {
            if (!fgets(buffer, max_buffer, input)) {
                if (!line.size()) {
                    return false;
                } else {
                    done = true;
                }
            }
            line += buffer;
            if (*line.rbegin() == '\n') {
                done = true;
            }
        }
        if (trim_newline) {
            trim_newline_chars(line);
        }
        return true;
    }

    class line_iterator
        : public boost::iterator_facade<
        line_iterator
        , std::string const
        , boost::forward_traversal_tag
        >
    {
    public:
        line_iterator()
            : m_file(0) 
        {}
    
        explicit line_iterator(FILE* input, bool trim_newline = false)
            : m_file(input) 
            , m_trim_newline(trim_newline)
        {}

    private:
        friend class boost::iterator_core_access;

        void increment() { 
            assert(m_file);
            if (!fast_getline(m_line, m_file, m_trim_newline)) {
                m_file = 0;
            }
        }

        bool equal(line_iterator const& other) const
        {
            return this->m_file == other.m_file;
        }

        std::string const& dereference() const {
            return m_line;
        }

        std::string m_line;
        FILE* m_file;
        bool m_trim_newline;
    };

    typedef std::pair<line_iterator, line_iterator> line_range_t;

    line_range_t lines(FILE* ifs, bool trim_newline = false) {
        return std::make_pair(line_iterator(ifs, trim_newline), line_iterator());
    }

    struct auto_file {

        auto_file(const char* name, const char* mode = "rb")
            : m_file(0)
        {
            m_file = fopen(name, mode);
            if(!m_file) {
                std::string msg("Unable to open file '");
                msg += name;
                msg += "'.";
                throw std::invalid_argument(msg);
                
            }
        }

        ~auto_file()
        {
            if(m_file) {
                fclose(m_file);
            }
        }
        
        FILE* get()
        {
            return m_file;
        }

    private:
        auto_file();
        auto_file( const auto_file & );
        auto_file & operator=( const auto_file & );

        FILE * m_file;
    };
    
    struct input_error : std::invalid_argument
    {
        input_error(std::string const& what)
            : invalid_argument(what)
        {}
    };

    template <typename T>
    void dispose(T& t)
    {
        T().swap(t);
    }

    uint64_t int2nat(int64_t x)
    {
        if (x < 0) {
            return -2 * x - 1;
        } else {
            return 2 * x;
        }
    }

    int64_t nat2int(uint64_t n)
    {
        if (n % 2) {
            return -int64_t((n + 1) / 2);
        } else {
            return n / 2;
        }
    }

}
}
