/* @file dur_commitjob.cpp */

/**
*    Copyright (C) 2009 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "dur_commitjob.h"

namespace mongo {
    namespace dur {

        void WriteIntent::absorb(const WriteIntent& other){
            dassert(overlaps(other));

            void* newEnd = max(end(), other.end());
            p = min(p, other.p);
            len = (char*)newEnd - (char*)p;

            dassert(contains(other));
        }

        void Writes::clear() { 
            _alreadyNoted.clear();
            _writes.clear();
            _ops.clear();
        }

        void Writes::insert(WriteIntent& wi){
            if (_writes.empty()){
                _writes.insert(wi);
                return;
            }

            typedef set<WriteIntent>::const_iterator iterator; // shorter

            iterator closest = _writes.lower_bound(wi);
            // closest.end() >= wi.end()

            if ((closest != _writes.end() && closest->overlaps(wi)) || // high end
                (closest != _writes.begin() && (--closest)->overlaps(wi))) // low end
            {
                if (closest->contains(wi))
                    return; // nothing to do

                // find overlapping range and merge into wi
                iterator   end(closest);
                iterator begin(closest);
                while (  end->overlaps(wi)) { wi.absorb(*end); ++end; if (end == _writes.end()) break; }  // look forwards
                while (begin->overlaps(wi)) { wi.absorb(*begin); if (begin == _writes.begin()) break; --begin; } // look backwards
                if (!begin->overlaps(wi)) ++begin; // make inclusive

                DEV { // ensure we're not deleting anything we shouldn't
                    for (iterator it(begin); it != end; ++it){
                        assert(wi.contains(*it));
                    }
                }

                _writes.erase(begin, end);
                _writes.insert(wi);

                DEV { // ensure there are no overlaps
                    for (iterator it(_writes.begin()), end(boost::prior(_writes.end())); it != end; ++it){
                        assert(!it->overlaps(*boost::next(it)));
                    }
                }
            }
            else { // no entries overlapping wi
                _writes.insert(closest, wi);
            }
        }


        /** note an operation other than a "basic write" */
        void CommitJob::noteOp(shared_ptr<DurOp> p) {
            DEV dbMutex.assertWriteLocked();
            dassert( cmdLine.dur );
            if( !_hasWritten ) {
                assert( !dbMutex._remapPrivateViewRequested );
                _hasWritten = true;
            }
            _wi._ops.push_back(p);
        }

        void CommitJob::reset() { 
            _hasWritten = false;
            _wi.clear();
            _ab.reset();
            _bytes = 0;
        }
    }
}
