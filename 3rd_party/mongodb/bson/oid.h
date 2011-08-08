// oid.h

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

#include "../util/hex.h"

namespace mongo {

#pragma pack(1)
    /**	Object ID type.
        BSON objects typically have an _id field for the object id.  This field should be the first 
        member of the object when present.  class OID is a special type that is a 12 byte id which 
        is likely to be unique to the system.  You may also use other types for _id's.
        When _id field is missing from a BSON object, on an insert the database may insert one 
        automatically in certain circumstances.

        Warning: You must call OID::newState() after a fork().

        Typical contents of the BSON ObjectID is a 12-byte value consisting of a 4-byte timestamp (seconds since epoch), 
        a 3-byte machine id, a 2-byte process id, and a 3-byte counter. Note that the timestamp and counter fields must 
        be stored big endian unlike the rest of BSON. This is because they are compared byte-by-byte and we want to ensure 
        a mostly increasing order.
    */
    class OID {
    public:
        struct MachineAndPid { 
            unsigned char _machineNumber[3];
            unsigned short _pid;
        };
    private:
        union {
            struct {
                // 12 bytes total
                unsigned char _time[4];
                MachineAndPid _machineAndPid;
                unsigned char _inc[3];
            };
            struct {
                long long a;
                unsigned b;
            };
            unsigned char data[12];
        };
    public:
		/** initialize to 'null' */
		void clear() { a = 0; b = 0; }

        const unsigned char *getData() const { return data; }

        bool operator==(const OID& r) const {
            return a==r.a&&b==r.b;
        }
        bool operator!=(const OID& r) const {
            return a!=r.a||b!=r.b;
        }

        /** The object ID output as 24 hex digits. */
        string str() const {
            return toHexLower(data, 12);
        }

        string toString() const { return str(); }

        static OID gen() { OID o; o.init(); return o; }
        
        /** sets the contents to a new oid / randomized value */
        void init();

        /** Set to the hex string value specified. */
        void init( string s );

        /** Set to the min/max OID that could be generated at given timestamp. */
        void init( Date_t date, bool max=false );

        time_t asTimeT();
        Date_t asDateT() { return asTimeT() * (long long)1000; }
        
        bool isSet() const { return a || b; }
        
        int compare( const OID& other ) const { return memcmp( data , other.data , 12 ); }
        
        bool operator<( const OID& other ) const { return compare( other ) < 0; }

        /** call this after a fork */
        static void justForked();

        static unsigned getMachineId();
        static void regenMachineId();
    };
#pragma pack()

    ostream& operator<<( ostream &s, const OID &o );
    inline StringBuilder& operator<< (StringBuilder& s, const OID& o) { return (s << o.str()); }

    /** Formatting mode for generating JSON from BSON.
        See <http://mongodb.onconfluence.com/display/DOCS/Mongo+Extended+JSON>
        for details.
    */
    enum JsonStringFormat {
        /** strict RFC format */
        Strict,
        /** 10gen format, which is close to JS format.  This form is understandable by
            javascript running inside the Mongo server via eval() */
        TenGen,
        /** Javascript JSON compatible */
        JS
    };

    inline ostream& operator<<( ostream &s, const OID &o ) {
        s << o.str();
        return s;
    }

}
