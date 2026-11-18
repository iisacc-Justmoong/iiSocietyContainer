#pragma once

#include "QtEventWait.h"
#include <memory>

namespace society_ctest {
// Preview workers hold a borrowed response pointer until they post completion.
// On a failed test, cancel and wait before allowing destruction. If the worker
// never finishes, leave its response alive until process exit rather than
// introducing a use-after-free into the failure path.
template<class Response, class Recorder>
bool retirePreviewResponse(std::unique_ptr<Response>& response, const Recorder* finished, int timeoutMs = 5000)
{
    if (!response) return true;
    if (!finished) { (void)response.release(); return false; }
    if (finished->isEmpty()) {
        response->cancel();
        if (!waitFor([&] { return !finished->isEmpty(); }, timeoutMs)) {
            (void)response.release();
            return false;
        }
    }
    return true;
}
} // namespace society_ctest
