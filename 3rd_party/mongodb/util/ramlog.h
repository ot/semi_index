// log.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "log.h"
#include "mongoutils/html.h"

namespace mongo {

    class RamLog : public Tee {
        enum { 
            N = 128,
            C = 256
        };
        char lines[N][C];
        unsigned h, n;

    public:
        RamLog() { 
            h = 0; n = 0;
            for( int i = 0; i < N; i++ )
                lines[i][C-1] = 0;
        }

        virtual void write(LogLevel ll, const string& str) {
            char *p = lines[(h+n)%N];
            if( str.size() < C )
                strcpy(p, str.c_str());
            else
                memcpy(p, str.c_str(), C-1);
            if( n < N ) n++;
            else h = (h+1) % N;
        }

        void get( vector<const char*>& v) const {
            for( unsigned x=0, i=h; x++ < n; i=(i+1)%N )
                v.push_back(lines[i]);
        }

        static int repeats(const vector<const char *>& v, int i) { 
            for( int j = i-1; j >= 0 && j+8 > i; j-- ) {
                if( strcmp(v[i]+20,v[j]+20) == 0 ) {
                    for( int x = 1; ; x++ ) {
                        if( j+x == i ) return j;
                        if( i+x>=(int) v.size() ) return -1;
                        if( strcmp(v[i+x]+20,v[j+x]+20) ) return -1;
                    }
                    return -1;
                }
            }
            return -1;
        }


        static string clean(const vector<const char *>& v, int i, string line="") { 
            if( line.empty() ) line = v[i];
            if( i > 0 && strncmp(v[i], v[i-1], 11) == 0 )
                return string("           ") + line.substr(11);
            return v[i];
        }

        static string color(string line) { 
            string s = str::after(line, "replSet ");
            if( str::startsWith(s, "warning") || startsWith(s, "error") )
                return html::red(line);
            if( str::startsWith(s, "info") ) {
                if( str::endsWith(s, " up\n") )
                    return html::green(line);
                else if( str::contains(s, " down ") || str::endsWith(s, " down\n") )
                    return html::yellow(line);
                return line; //html::blue(line);
            }
            
            return line;
        }

        /* turn http:... into an anchor */
        string linkify(const char *s) { 
            const char *p = s;
            const char *h = strstr(p, "http://");
            if( h == 0 ) return s;
	  
            const char *sp = h + 7;
            while( *sp && *sp != ' ' ) sp++;

            string url(h, sp-h);
            stringstream ss;
            ss << string(s, h-s) << "<a href=\"" << url << "\">" << url << "</a>" << sp;
            return ss.str();
        }

        void toHTML(stringstream& s) {
            vector<const char*> v;
            get( v );

            bool first = true;
            s << "<pre>\n";
            for( int i = 0; i < (int)v.size(); i++ ) {
                assert( strlen(v[i]) > 20 );
                int r = repeats(v, i);
                if( r < 0 ) {
                    s << color( linkify( clean(v,i).c_str() ) );
                } 
                else {
                    stringstream x;
                    x << string(v[i], 0, 20);
                    int nr = (i-r);
                    int last = i+nr-1;
                    for( ; r < i ; r++ ) x << '.';
                    if( 1 ) { 
                        stringstream r; 
                        if( nr == 1 ) r << "repeat last line";
                        else r << "repeats last " << nr << " lines; ends " << string(v[last]+4,0,15);
                        first = false; s << html::a("", r.str(), clean(v,i,x.str()));
                    }
                    else s << x.str();
                    s << '\n';
                    i = last;
                }
            }
            s << "</pre>\n";
        }
        

    };

}
