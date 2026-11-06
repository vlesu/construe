// Local session factory (architecture.md §2).
#include "api/local_session.h"

#include "kernel_occt/occt_kernel.h"

namespace construe::api {

std::unique_ptr<Session> createLocalSession(Callbacks callbacks) {
    return std::make_unique<Session>(kernel::createOcctKernel(), std::move(callbacks));
}

}  // namespace construe::api
