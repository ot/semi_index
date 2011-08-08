// @file paths.h
// file paths and directory handling

/*    Copyright 2010 10gen Inc.
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

#include "mongoutils/str.h"

using namespace mongoutils;

namespace mongo {

    extern string dbpath; 

    /** this is very much like a boost::path.  however, we define a new type to get some type 
        checking.  if you want to say 'my param MUST be a relative path", use this.
    */
    struct RelativePath { 
        string _p;

        static RelativePath fromRelativePath(string f) { 
            RelativePath rp;
            rp._p = f;
            return rp;
        }

        /** from a full path */
        static RelativePath fromFullPath(path f) { 
            string fullpath = f.string();
            string relative = str::after(fullpath, dbpath);
            uassert(13600, 
                    str::stream() << "file path is not under the db path? " << fullpath << ' ' << dbpath, 
                    relative != fullpath);
            if( str::startsWith(relative, "/") || str::startsWith(relative, "\\") ) { 
                relative.erase(0, 1);
            }
            RelativePath rp;
            rp._p = relative;
            return rp;
        }

        string toString() const { return _p; }

        inline bool operator!=(const RelativePath& r) const { return _p != r._p; }
        inline bool operator==(const RelativePath& r) const { return _p == r._p; }

        string asFullPath() const { 
            path x(dbpath);
            x /= _p;
            return x.string();
        } 

    };

}
