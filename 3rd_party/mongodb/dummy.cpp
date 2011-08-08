#include "pch.h"

namespace mongo {

    void dbexit(ExitCode returnCode, const char *whyMsg, bool tryToGetLock)
    {
        puts(whyMsg);
        abort();
    }

}

int do_md5_test() 
{
    return 0;
}
