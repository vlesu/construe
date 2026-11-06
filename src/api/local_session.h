// Local session factory (architecture.md §2): the only place where the kernel implementation is chosen.
// The client depends on api and this factory, but not on kernel_occt.
#pragma once

#include "api/session.h"

#include <memory>

namespace construe::api {

std::unique_ptr<Session> createLocalSession(Callbacks callbacks = {});

}  // namespace construe::api
