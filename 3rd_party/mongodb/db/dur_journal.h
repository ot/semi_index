// @file dur_journal.h

/**
*    Copyright (C) 2010 10gen Inc.
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

#pragma once

namespace mongo {
    class AlignedBuilder;

    namespace dur {

        /** true if ok to cleanup journal files at termination. otherwise, files journal will be retained. 
        */
        extern bool okToCleanUp;

        /** at termination after db files closed & fsynced */
        void journalCleanup();

        /** assure journal/ dir exists. throws */
        void journalMakeDir();

        /** check if time to rotate files; assure a file is open. 
            done separately from the journal() call as we can do this part
            outside of lock.
         */
        void journalRotate();

        /** write/append to journal file *
            @param buf - a buffer that will be written to the journal.
            will not return until on disk
        */
        void journal(const AlignedBuilder& buf);

        /** flag that something has gone wrong during writing to the journal
            (not for recovery mode) 
        */
        void journalingFailure(const char *msg);

        /** never throws.  
            @return true if there are any journal files in the journal dir. 
        */
        bool haveJournalFiles();
    }
}
